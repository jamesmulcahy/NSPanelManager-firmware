"""Library: the dev stack's web side, used the way its web UI uses it. StackManager accepts and
deletes panels through it.

The UI learns about panels from MQTTManager's STOMP-over-websocket server
(/websocket/stomp, proxied by nginx):

  entity_states          {"nspanels": [{"id": 3, "mac": "C0:49:EF:..."}, ...]}  (retained)
  nspanel/<mac>/status   {"state": "awaiting_accept", "accepted": false, ...}

and acts through Django's REST API. Django's CSRF check is double-submit (the
X-CSRFToken header must match the csrftoken cookie), so the harness picks its
own token and sends it as both:

  POST   /rest/nspanel/<id>/accept   {"room_id": 1}
  DELETE /rest/nspanel/<id>
  GET    /rest/rooms
"""

import json
import logging
import secrets
import string
import threading
import urllib.request
from typing import Callable, Dict, List, Optional

import websocket

from .waiting import wait_for

log = logging.getLogger("stack-web")


class Stomp:
    """A minimal STOMP 1.2 client: subscribe to topics and keep the latest body of each."""

    def __init__(self, url: str):
        self.values: Dict[str, str] = {}
        self._cond = threading.Condition()
        self._next_id = 0
        self._connected = threading.Event()
        self._ws = websocket.WebSocketApp(url, on_open=self._on_open,
                                          on_message=self._on_message,
                                          on_error=lambda ws, e: log.warning("STOMP websocket error: %s", e))
        self._thread = threading.Thread(target=self._ws.run_forever, name="stomp", daemon=True)
        self._thread.start()
        if not self._connected.wait(15):
            raise RuntimeError("no STOMP CONNECTED from %s" % url)

    def subscribe(self, destination: str) -> None:
        with self._cond:
            sub_id = "sub-%d" % self._next_id
            self._next_id += 1
        self._send("SUBSCRIBE", {"id": sub_id, "destination": destination})

    def wait_for(self, destination: str, predicate: Callable[[dict], bool], timeout: float,
                 abort_check: Optional[Callable[[], None]] = None) -> dict:
        found: List[dict] = []

        def match():
            body = self.values.get(destination)
            if body:
                try:
                    value = json.loads(body)
                except ValueError:
                    return False
                if predicate(value):
                    found.append(value)
                    return True
            return False

        with self._cond:
            if not wait_for(self._cond, match, timeout, abort_check):
                raise TimeoutError("STOMP %s: no matching value within %.0fs; last %r"
                                   % (destination, timeout, self.values.get(destination, "")[:200]))
        return found[0]

    def close(self) -> None:
        self._ws.close()
        self._thread.join(5)

    def _send(self, command: str, headers: Dict[str, str], body: str = "") -> None:
        frame = command + "\n" + "".join("%s:%s\n" % kv for kv in headers.items()) + "\n" + body + "\0"
        self._ws.send(frame)

    def _on_open(self, ws) -> None:
        self._send("CONNECT", {"accept-version": "1.2", "host": "/", "heart-beat": "0,0"})

    def _on_message(self, ws, data) -> None:
        if isinstance(data, bytes):
            data = data.decode("utf-8", errors="replace")
        for raw in data.split("\0"):
            raw = raw.lstrip("\r\n")
            if not raw:
                continue
            head, _, body = raw.partition("\n\n")
            lines = head.split("\n")
            headers = dict(line.split(":", 1) for line in lines[1:] if ":" in line)
            if lines[0] == "CONNECTED":
                self._connected.set()
            elif lines[0] == "MESSAGE" and "destination" in headers:
                with self._cond:
                    self.values[headers["destination"]] = body
                    self._cond.notify_all()
            elif lines[0] == "ERROR":
                log.warning("STOMP error: %s %s", headers.get("message", ""), body)


class StackWeb:
    def __init__(self, web_url: str):
        self.web_url = web_url
        self._csrf = "".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(32))
        self._stomp: Optional[Stomp] = None

    @property
    def stomp(self) -> Stomp:
        if self._stomp is None:
            self._stomp = Stomp(self.web_url.replace("http", "ws", 1) + "/websocket/stomp")
            self._stomp.subscribe("entity_states")
        return self._stomp

    # ---- what the UI shows -------------------------------------------------

    def wait_for_panel_id(self, mac: str, timeout: float = 60, abort_check=None) -> int:
        """The panel's id, once MQTTManager has seen it (accepted or not)."""
        states = self.stomp.wait_for("entity_states", lambda v: any(p["mac"] == mac for p in v.get("nspanels", [])),
                                     timeout, abort_check)
        return next(p["id"] for p in states["nspanels"] if p["mac"] == mac)

    def wait_for_panel_status(self, mac: str, predicate: Callable[[dict], bool] = lambda s: True,
                              timeout: float = 60, abort_check=None) -> dict:
        topic = "nspanel/%s/status" % mac
        if topic not in self.stomp.values:
            self.stomp.subscribe(topic)
        return self.stomp.wait_for(topic, predicate, timeout, abort_check)

    def rooms(self) -> List[dict]:
        return self._request("GET", "/rest/rooms")["rooms"]

    # ---- what the user does ------------------------------------------------

    def accept_panel(self, panel_id: int, room_id: Optional[int] = None) -> None:
        if room_id is None:
            room_id = min(r["id"] for r in self.rooms())
        self._request("POST", "/rest/nspanel/%d/accept" % panel_id, {"room_id": room_id})

    def delete_panel(self, panel_id: int) -> None:
        self._request("DELETE", "/rest/nspanel/%d" % panel_id)

    def add_light(self, room_id: int, name: str, ceiling: bool = True) -> int:
        """Add a dimmable Home Assistant light to the room, as the UI's "add light"
        dialog does, and return its entity id. The stack has no Home Assistant, so
        the light is never really switched, but the manager counts it in the room's
        status, and the panel only sends light commands for a room with lights."""
        page = next(p for p in self._request("GET", "/rest/rooms/%d/entities_pages" % room_id)["entities_pages"]
                    if p["type"] == "entity")
        position = len(page["entities"])
        self._request("PUT", "/rest/entities/lights", {"values": {
            "room_id": room_id,
            "entities_page_id": page["id"],
            "room_view_position": position,
            "controller": "home_assistant",
            "type": "light",
            "friendly_name": name,
            "can_color_temperature": True,
            "can_dim": True,
            "can_rgb": False,
            "controlled_by_nspanel_main_page": True,
            "home_assistant_name": "light." + name.replace("-", "_"),
            "is_ceiling_light": ceiling,
            "openhab_item_color_temp": "",
            "openhab_item_dimmer": "",
            "openhab_item_rgb": "",
        }})
        # The PUT does not return the new id; the name is unique to this test.
        entities = self._request("GET", "/rest/rooms/%d/entities" % room_id)["entities"]
        return next(e["id"] for e in entities if e.get("friendly_name") == name)

    def delete_entity(self, entity_id: int) -> None:
        self._request("DELETE", "/rest/entities/%d" % entity_id)

    def restart_manager(self) -> None:
        self._request("GET", "/api/restart_mqtt_manager")

    def close(self) -> None:
        if self._stomp is not None:
            self._stomp.close()
            self._stomp = None

    # ---- internals ---------------------------------------------------------

    def _request(self, method: str, path: str, body: Optional[dict] = None, timeout: float = 30):
        headers = {}
        data = None
        if method != "GET":
            headers["X-CSRFToken"] = self._csrf
            headers["Cookie"] = "csrftoken=" + self._csrf
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.web_url + path, data=data, method=method, headers=headers)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            payload = r.read()
            if r.headers.get_content_type() == "application/json":
                return json.loads(payload)
            return payload
