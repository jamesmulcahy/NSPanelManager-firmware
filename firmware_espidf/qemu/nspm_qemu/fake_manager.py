"""A stand-in for NSPanelManager's MQTTManager, speaking to the panel over the in-process broker.

It implements the registration handshake and serves config and room state,
using the same topics and retained-message conventions as the real manager
(docker/MQTTManager in the NSPanelManager repo):

  panel   -> nspanel/mqttmanager/command            register_request (JSON)
  manager -> nspanel/<friendly_name>/command         register_accept (JSON)
  manager -> nspanel/<mac>/config                    NSPanelConfig (retained)
  manager -> nspanel/mqttmanager_<addr>/status/status          "online" (retained)
  manager -> nspanel/mqttmanager_<addr>/room/<id>/state        NSPanelRoomStatus (retained)
  manager -> nspanel/mqttmanager_<addr>/all_rooms_status       NSPanelRoomStatus (retained)
  manager -> nspanel/mqttmanager_<addr>/entity_pages/<id>/state NSPanelRoomEntitiesPage (retained)
  panel   -> nspanel/mqttmanager_<addr>/command      NSPanelMQTTManagerCommand
"""

import json
import logging
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

from .broker import Broker, Message
from .proto import nspanel as pb
from .waiting import wait_for

log = logging.getLogger("manager")

# The host as seen from QEMU's user-mode network.
QEMU_HOST_ADDRESS = "10.0.2.2"


@dataclass
class Room:
    id: int
    name: str
    num_ceiling_lights: int = 1
    num_table_lights: int = 1
    num_ceiling_lights_on: int = 0
    num_table_lights_on: int = 0
    ceiling_lights_dim_level: int = 0
    table_lights_dim_level: int = 0
    color_temperature: int = 50
    entity_page_ids: List[int] = field(default_factory=list)
    scene_page_ids: List[int] = field(default_factory=list)

    def status(self) -> "pb.NSPanelRoomStatus":
        lights_on = self.num_ceiling_lights_on + self.num_table_lights_on
        levels = [lvl for lvl, on in ((self.ceiling_lights_dim_level, self.num_ceiling_lights_on),
                                       (self.table_lights_dim_level, self.num_table_lights_on)) if on]
        return pb.NSPanelRoomStatus(
            id=self.id,
            name=self.name,
            average_dim_level=sum(levels) // len(levels) if lights_on else 0,
            ceiling_lights_dim_level=self.ceiling_lights_dim_level,
            table_lights_dim_level=self.table_lights_dim_level,
            average_color_temperature=self.color_temperature,
            ceiling_lights_color_temperature_value=self.color_temperature,
            table_lights_color_temperature_value=self.color_temperature,
            num_ceiling_lights=self.num_ceiling_lights,
            num_table_lights=self.num_table_lights,
            num_ceiling_lights_on=self.num_ceiling_lights_on,
            num_table_lights_on=self.num_table_lights_on,
            entity_page_ids=self.entity_page_ids,
        )


def default_rooms() -> List[Room]:
    return [
        Room(1, "Office", entity_page_ids=[11]),
        Room(2, "Kitchen", entity_page_ids=[21]),
    ]


def default_entity_pages() -> Dict[int, "pb.NSPanelRoomEntitiesPage"]:
    Slot = pb.NSPanelRoomEntitiesPage.EntitySlot
    pages = {}
    for page_id, header in ((11, "Office"), (21, "Kitchen")):
        pages[page_id] = pb.NSPanelRoomEntitiesPage(
            id=page_id,
            page_type=4,
            header_text=header,
            entities=[
                Slot(room_view_position=0, name="Desk lamp", icon="s", pco=65535, pco2=65535,
                     type=Slot.ENTITY_TYPE_LIGHT, id=page_id * 10 + 1),
                Slot(room_view_position=1, name="Fan", icon="t", pco=65535, pco2=65535,
                     type=Slot.ENTITY_TYPE_SWITCH, id=page_id * 10 + 2),
            ],
        )
    return pages


class FakeManager:
    def __init__(self, broker: Broker, address: str = QEMU_HOST_ADDRESS, port: int = 8000,
                 rooms: Optional[List[Room]] = None, nspanel_id: int = 1):
        self.broker = broker
        self.address = address
        self.port = port
        self.nspanel_id = nspanel_id
        self.rooms: Dict[int, Room] = {r.id: r for r in (rooms or default_rooms())}
        self.entity_pages = default_entity_pages()
        self.accept_registrations = True
        self.register_requests: List[dict] = []
        self.commands: List["pb.NSPanelMQTTManagerCommand"] = []
        self._cond = threading.Condition()
        # Called while waiting; raises to abort the wait (Rig sets QemuPanel.check_alive).
        self.abort_check: Optional[Callable[[], None]] = None
        self.panel_mac: Optional[str] = None
        self.panel_name: Optional[str] = None
        self.config = self.default_config()
        broker.add_listener(self._on_message)

    # ---- topics ------------------------------------------------------------

    @property
    def base(self) -> str:
        return "nspanel/mqttmanager_%s" % self.address

    @property
    def command_topic(self) -> str:
        return self.base + "/command"

    def config_topic(self, mac: Optional[str] = None) -> str:
        return "nspanel/%s/config" % (mac or self.panel_mac)

    # ---- state the manager serves -----------------------------------------

    def default_config(self) -> "pb.NSPanelConfig":
        # name is left empty and filled in from the panel's register_request, as
        # the real manager does for a panel that has not been renamed. Set it to
        # rename the panel.
        return pb.NSPanelConfig(
            nspanel_id=self.nspanel_id,
            default_room=min(self.rooms),
            default_page=pb.NSPanelConfig.HOME,
            screensaver_activation_timeout=60000,
            screensaver_mode=pb.NSPanelConfig.NO_SCREENSAVER,
            min_button_push_time=50,
            button_long_press_time=5000,
            special_mode_trigger_time=300,
            special_mode_release_time=5000,
            screen_dim_level=100,
            screensaver_dim_level=10,
            room_infos=[pb.NSPanelConfig.RoomInfo(room_id=r.id, entity_page_ids=r.entity_page_ids,
                                                  scene_page_ids=r.scene_page_ids) for r in self.rooms.values()],
            default_light_brightess=50,
            raise_light_level_to_100_above=95,
        )

    def publish_online(self, online: bool = True) -> None:
        self.broker.publish(self.base + "/status/status", "online" if online else "offline", retain=True)

    def publish_config(self, config: Optional["pb.NSPanelConfig"] = None) -> None:
        if config is not None:
            self.config = config
        self.broker.publish(self.config_topic(), self.config.SerializeToString(), retain=True)

    def publish_room(self, room_id: int) -> None:
        self.broker.publish("%s/room/%d/state" % (self.base, room_id), self.rooms[room_id].status().SerializeToString(), retain=True)

    def publish_all_rooms(self) -> None:
        rooms = list(self.rooms.values())
        merged = Room(0, "All rooms",
                      num_ceiling_lights=sum(r.num_ceiling_lights for r in rooms),
                      num_table_lights=sum(r.num_table_lights for r in rooms),
                      num_ceiling_lights_on=sum(r.num_ceiling_lights_on for r in rooms),
                      num_table_lights_on=sum(r.num_table_lights_on for r in rooms))
        self.broker.publish(self.base + "/all_rooms_status", merged.status().SerializeToString(), retain=True)

    def publish_entity_page(self, page_id: int) -> None:
        self.broker.publish("%s/entity_pages/%d/state" % (self.base, page_id), self.entity_pages[page_id].SerializeToString(), retain=True)

    def publish_everything(self) -> None:
        self.publish_online()
        for room_id in self.rooms:
            self.publish_room(room_id)
        self.publish_all_rooms()
        for page_id in self.entity_pages:
            self.publish_entity_page(page_id)
        self.publish_config()

    # ---- observation -------------------------------------------------------

    def wait_for_registration(self, timeout: float = 60, since: int = 0) -> dict:
        with self._cond:
            ok = wait_for(self._cond, lambda: len(self.register_requests) > since, timeout, self.abort_check)
            if not ok:
                raise TimeoutError("panel did not send register_request within %.0fs" % timeout)
            return self.register_requests[since]

    def wait_for_command(self, predicate: Callable[["pb.NSPanelMQTTManagerCommand"], bool] = lambda c: True,
                         timeout: float = 30, since: int = 0) -> "pb.NSPanelMQTTManagerCommand":
        found: List = []

        def match():
            for c in self.commands[since:]:
                if predicate(c):
                    found.append(c)
                    return True
            return False

        with self._cond:
            if not wait_for(self._cond, match, timeout, self.abort_check):
                raise TimeoutError("manager did not receive a matching command within %.0fs; got %s"
                                   % (timeout, [c.WhichOneof("CommandData") for c in self.commands[since:]]))
        return found[0]

    def command_mark(self) -> int:
        with self._cond:
            return len(self.commands)

    def registration_mark(self) -> int:
        with self._cond:
            return len(self.register_requests)

    # ---- internals ---------------------------------------------------------

    def _on_message(self, msg: Message) -> None:
        if msg.sender == "<test>":
            return
        if msg.topic == "nspanel/mqttmanager/command":
            self._on_register_request(msg)
        elif msg.topic == self.command_topic:
            cmd = pb.NSPanelMQTTManagerCommand()
            try:
                cmd.ParseFromString(msg.payload)
            except Exception:
                log.warning("undecodable command on %s: %r", msg.topic, msg.payload[:64])
                return
            log.info("command from panel: %s", cmd.WhichOneof("CommandData"))
            with self._cond:
                self.commands.append(cmd)
                self._cond.notify_all()

    def _on_register_request(self, msg: Message) -> None:
        try:
            request = json.loads(msg.payload)
        except ValueError:
            log.warning("unparseable register_request: %r", msg.payload[:64])
            return
        if request.get("command") != "register_request":
            return
        with self._cond:
            self.register_requests.append(request)
            self._cond.notify_all()
        if not self.accept_registrations:
            return
        self.panel_mac = request["mac_origin"]
        self.panel_name = request["friendly_name"]
        if not self.config.name:
            self.config.name = self.panel_name
        accept = json.dumps({
            "command": "register_accept",
            "address": self.address,
            "port": self.port,
            "config_topic": self.config_topic(),
        })
        # Publish state before the accept so it is retained by the time the
        # panel subscribes, as it would be against a long-running manager.
        self.publish_everything()
        self.broker.publish("nspanel/%s/command" % self.panel_name, accept)
        self.broker.publish("nspanel/%s/command" % self.panel_mac, accept)
