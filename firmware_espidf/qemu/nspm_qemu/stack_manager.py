"""Library: the real NSPanelManager, from the NSPanelManager repo's dev stack (docker/dev), seen
from the harness. Rig uses it in place of FakeManager for tests marked `stack`.

Where FakeManager plays the manager, StackManager only watches it: the stack's
MQTTManager answers the panel, and the harness reads the same MQTT traffic
FakeManager would have produced, plus the stack's web API for actions a user
takes in the web UI.

A panel the stack has not seen before registers as "awaiting accept", and
`accept()` accepts it as a user would in the web UI (see stack_web.py), so any
number of panels with any MACs can join. `forget()` deletes it again. Topics,
as in fake_manager.py:

  panel   -> nspanel/mqttmanager/command                 register_request (JSON)
  manager -> nspanel/<mac>/command                       register_accept (JSON)
  manager -> nspanel/<mac>/config                        NSPanelConfig (retained)
  manager -> nspanel/mqttmanager_<addr>/status/status    "online" (retained)
  manager -> nspanel/mqttmanager_<addr>/room/<id>/state  NSPanelRoomStatus (retained)
  panel   -> nspanel/mqttmanager_<addr>/command          NSPanelMQTTManagerCommand
"""

import json
import logging
import os
import threading
from dataclasses import dataclass
from typing import Callable, List, Optional, Tuple

from .broker import Broker, Message
from .proto import nspanel as pb
from .stack_web import StackWeb
from .waiting import wait_for

log = logging.getLogger("stack")


@dataclass
class Stack:
    """Where a running dev stack is, from the host. `nspm-dev up` prints the broker port."""
    mqtt_host: str
    mqtt_port: int
    web_url: str

    @classmethod
    def from_env(cls) -> Optional["Stack"]:
        """NSPM_STACK_MQTT=host:port (required) and NSPM_STACK_WEB (default http://127.0.0.1:8010)."""
        mqtt = os.environ.get("NSPM_STACK_MQTT")
        if not mqtt:
            return None
        host, _, port = mqtt.rpartition(":")
        return cls(host or "127.0.0.1", int(port), os.environ.get("NSPM_STACK_WEB", "http://127.0.0.1:8010").rstrip("/"))

    @property
    def mqtt(self) -> Tuple[str, int]:
        return self.mqtt_host, self.mqtt_port


class StackManager:
    def __init__(self, broker: Broker, stack: Stack, panel_mac: str):
        self.broker = broker
        self.stack = stack
        self.web = StackWeb(stack.web_url)
        self.panel_mac = panel_mac
        self.panel_id: Optional[int] = None  # the manager's id for the panel, from the web UI's view
        self.address: Optional[str] = None  # the manager's MANAGER_ADDRESS setting, from register_accept
        self.register_requests: List[dict] = []
        self.accepts: List[dict] = []
        self.configs: List["pb.NSPanelConfig"] = []
        self.commands: List["pb.NSPanelMQTTManagerCommand"] = []
        self._cond = threading.Condition()
        # Called while waiting; raises to abort the wait (Rig sets QemuPanel.check_alive).
        self.abort_check: Optional[Callable[[], None]] = None
        broker.add_listener(self._on_message)

    # ---- what the manager has told the panel -------------------------------

    @property
    def config(self) -> Optional["pb.NSPanelConfig"]:
        with self._cond:
            return self.configs[-1] if self.configs else None

    @property
    def nspanel_id(self) -> int:
        config = self.config
        if config is None:
            raise RuntimeError("the manager has not sent this panel a config yet")
        return config.nspanel_id

    @property
    def base(self) -> str:
        if self.address is None:
            raise RuntimeError("the manager has not accepted this panel yet")
        return "nspanel/mqttmanager_%s" % self.address

    def room_status(self, room_id: int, since: int = 0, timeout: float = 30) -> "pb.NSPanelRoomStatus":
        msg = self.broker.wait_for_message("%s/room/%d/state" % (self.base, room_id), timeout=timeout, since=since)
        status = pb.NSPanelRoomStatus()
        status.ParseFromString(msg.payload)
        return status

    # ---- waits, with the same names and marks as FakeManager ----------------

    def wait_for_registration(self, timeout: float = 60, since: int = 0) -> dict:
        return self._wait(self.register_requests, since, timeout, "panel did not send register_request")

    def wait_for_accept(self, timeout: float = 60, since: int = 0) -> dict:
        return self._wait(self.accepts, since, timeout, "manager did not send register_accept")

    def wait_for_config(self, predicate: Callable[["pb.NSPanelConfig"], bool] = lambda c: True,
                        timeout: float = 60, since: int = 0) -> "pb.NSPanelConfig":
        return self._wait(self.configs, since, timeout, "manager did not send a matching config", predicate)

    def wait_for_command(self, predicate: Callable[["pb.NSPanelMQTTManagerCommand"], bool] = lambda c: True,
                         timeout: float = 30, since: int = 0) -> "pb.NSPanelMQTTManagerCommand":
        return self._wait(self.commands, since, timeout, "panel sent no matching command", predicate)

    def registration_mark(self) -> int:
        with self._cond:
            return len(self.register_requests)

    def accept_mark(self) -> int:
        with self._cond:
            return len(self.accepts)

    def config_mark(self) -> int:
        with self._cond:
            return len(self.configs)

    def command_mark(self) -> int:
        with self._cond:
            return len(self.commands)

    # ---- actions a user would take in the web UI --------------------------

    def accept(self, room_id: Optional[int] = None, timeout: float = 60) -> None:
        """Accept the panel once the UI shows it as awaiting accept. A panel the
        stack already accepted (e.g. on an earlier boot) is left alone."""
        self.panel_id = self.web.wait_for_panel_id(self.panel_mac, timeout, self.abort_check)
        status = self.web.wait_for_panel_status(self.panel_mac, lambda s: "accepted" in s, timeout, self.abort_check)
        if not status["accepted"]:
            log.info("accepting panel %d (%s)", self.panel_id, self.panel_mac)
            self.web.accept_panel(self.panel_id, room_id)

    def forget(self) -> None:
        """Delete the panel from the stack, as the UI's delete button does. Also
        works after a test that failed before learning the panel's id, so it
        does not leave the panel behind as awaiting accept."""
        panel_id = self.panel_id
        if panel_id is None:
            try:
                panel_id = self.web.wait_for_panel_id(self.panel_mac, timeout=5)
            except Exception:
                return  # the stack never saw it
        self.web.delete_panel(panel_id)
        self.panel_id = None

    def restart(self) -> None:
        """Restart MQTTManager, as saving settings in the web UI does."""
        self.web.restart_manager()

    def close(self) -> None:
        self.web.close()

    # ---- internals ---------------------------------------------------------

    def _wait(self, items: list, since: int, timeout: float, what: str, predicate=lambda x: True):
        found: List = []

        def match():
            for item in items[since:]:
                if predicate(item):
                    found.append(item)
                    return True
            return False

        with self._cond:
            if not wait_for(self._cond, match, timeout, self.abort_check):
                raise TimeoutError("%s within %.0fs" % (what, timeout))
        return found[0]

    def _append(self, items: list, item) -> None:
        with self._cond:
            items.append(item)
            self._cond.notify_all()

    def _on_message(self, msg: Message) -> None:
        if msg.topic == "nspanel/mqttmanager/command":
            request = _json(msg)
            if request and request.get("command") == "register_request" and request.get("mac_origin") == self.panel_mac:
                self._append(self.register_requests, request)
        elif msg.topic == "nspanel/%s/command" % self.panel_mac:
            accept = _json(msg)
            if accept and accept.get("command") == "register_accept":
                self.address = accept["address"]
                self._append(self.accepts, accept)
        elif msg.topic == "nspanel/%s/config" % self.panel_mac:
            # A retained config from an earlier run is replayed when the harness
            # subscribes, before this panel has booted. It is still what the
            # manager would send, so it is recorded like any other.
            config = pb.NSPanelConfig()
            try:
                config.ParseFromString(msg.payload)
            except Exception:
                log.warning("undecodable config: %r", msg.payload[:64])
                return
            self._append(self.configs, config)
        elif self.address is not None and msg.topic == self.base + "/command":
            cmd = pb.NSPanelMQTTManagerCommand()
            try:
                cmd.ParseFromString(msg.payload)
            except Exception:
                return  # the stack's simulated panels share this topic; not ours to judge
            config = self.config
            if config is not None and cmd.nspanel_id == config.nspanel_id:
                log.info("command from panel: %s", cmd.WhichOneof("CommandData"))
                self._append(self.commands, cmd)


def _json(msg: Message) -> Optional[dict]:
    try:
        value = json.loads(msg.payload)
    except ValueError:
        return None
    return value if isinstance(value, dict) else None
