"""Fake service: a stand-in for NSPanelManager's MQTTManager. It registers the panel and serves
its config, room state and weather over the test's broker, and records the commands the panel
sends back.

It implements the registration handshake and serves config and room state,
using the same topics and retained-message conventions as the real manager
(docker/MQTTManager in the NSPanelManager repo):

  panel   -> nspanel/mqttmanager/command            register_request (JSON)
  manager -> nspanel/<friendly_name>/command         register_accept (JSON)
  manager -> nspanel/<mac>/command                   register_accept (JSON, the same message)
  manager -> nspanel/<mac>/config                    NSPanelConfig (retained)
  manager -> nspanel/mqttmanager_<addr>/status/status          "online" (retained)
  manager -> nspanel/mqttmanager_<addr>/room/<id>/state        NSPanelRoomStatus (retained)
  manager -> nspanel/mqttmanager_<addr>/all_rooms_status       NSPanelRoomStatus (retained)
  manager -> nspanel/mqttmanager_<addr>/entity_pages/<id>/state NSPanelRoomEntitiesPage (retained)
  manager -> nspanel/mqttmanager_<addr>/status/weather          NSPanelWeatherUpdate (retained)
  panel   -> nspanel/mqttmanager_<addr>/command      NSPanelMQTTManagerCommand; its lvgl_look (a look
                                                     chosen on the panel) is, with confirm_looks,
                                                     answered as the manager does: the config again,
                                                     with that look
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
    # How many lights can change colour temperature. None: not sent at all, as by a manager from
    # before the field (the panel must then show the colour-temperature slider).
    num_color_temperature_lights: Optional[int] = None
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

    def status_bytes(self) -> bytes:
        """The status as published: with the colour-temperature count, whether or not the pinned
        schema has its fields (num_color_temperature_lights = 14, color_temperature_lights_known = 15)."""
        status = self.status()
        if self.num_color_temperature_lights is None:
            return status.SerializeToString()
        fields = status.DESCRIPTOR.fields_by_name
        if "color_temperature_lights_known" in fields:
            status.num_color_temperature_lights = self.num_color_temperature_lights
            status.color_temperature_lights_known = True
            return status.SerializeToString()
        extra = _varint_field(15, 1)
        if self.num_color_temperature_lights:
            extra = _varint_field(14, self.num_color_temperature_lights) + extra
        return status.SerializeToString() + extra


def default_rooms() -> List[Room]:
    return [
        Room(1, "Office", entity_page_ids=[11]),
        Room(2, "Kitchen", entity_page_ids=[21]),
    ]


def default_weather() -> "pb.NSPanelWeatherUpdate":
    """Five days of forecast. Icons are characters of the TFT's weather icon fonts."""
    update = pb.NSPanelWeatherUpdate(
        current_weather_icon="c", current_temperature_string="21°", current_maxmin_temperature="24°/15°",
        current_wind_string="12 km/h", sunrise_string="06:41", sunset_string="18:20",
        current_precipitation_string="10%")
    for day, icon in zip(["Mon", "Tue", "Wed", "Thu", "Fri"], "abcde"):
        update.forecast_items.add(display_string=day, weather_icon=icon, precipitation_string="20%",
                                  temperature_maxmin_string="22°/14°", wind_string="10 km/h")
    return update


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


def _fields(data: bytes):
    """(number, wire type, value) of each top-level protobuf field; value is bytes for length-delimited
    fields and an int for varints (the only two wire types these messages use)."""
    def varint(i: int):
        n = shift = 0
        while True:
            byte = data[i]
            n |= (byte & 0x7F) << shift
            i += 1
            if not byte & 0x80:
                return n, i
            shift += 7
    i = 0
    while i < len(data):
        key, i = varint(i)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, i = varint(i)
        elif wire == 2:
            size, i = varint(i)
            value, i = data[i:i + size], i + size
        else:
            raise ValueError("wire type %d" % wire)
        yield number, wire, value


def lvgl_look_of(command_payload: bytes) -> Optional[Dict[str, str]]:
    """NSPanelMQTTManagerCommand.lvgl_look (field 11: LvglLook {theme = 1, background = 2}) as
    {"theme", "background"}, decoded by hand (the pinned .proto may not have it), or None."""
    for number, wire, value in _fields(command_payload):
        if number == 11 and wire == 2:
            look = {"theme": "", "background": ""}
            for inner, inner_wire, inner_value in _fields(value):
                if inner_wire == 2 and inner in (1, 2):
                    look["theme" if inner == 1 else "background"] = inner_value.decode()
            return look
    return None


def _varint(n: int) -> bytes:
    out = b""
    while True:
        byte, n = n & 0x7F, n >> 7
        if n:
            out += bytes([byte | 0x80])
        else:
            return out + bytes([byte])


def _string_field(number: int, value: str) -> bytes:
    """A protobuf string field, encoded by hand (for fields the pinned .proto may not have yet)."""
    data = value.encode()
    return _varint(number << 3 | 2) + _varint(len(data)) + data


def _varint_field(number: int, value: int) -> bytes:
    """A protobuf int32/bool field (non-negative), encoded by hand, as _string_field()."""
    return _varint(number << 3) + _varint(value)


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
        # The LVGL look the config carries (NSPanelConfig.lvgl_theme 49, lvgl_background 50): None for
        # not managed, as the manager sends for a panel left on "Keep the panel's own".
        self.lvgl_look: Optional[Dict[str, str]] = None
        # Answer a look report as MQTTManager does (NSPanel::_handle_lvgl_look_report).
        self.confirm_looks = True
        self.look_reports: List[dict] = []
        # JSON commands ("reboot", ...) to send on the MAC command topic straight after the next
        # register_accept, as a manager does when someone presses a button while the panel registers.
        # Sent once, then cleared.
        self.commands_after_accept: List[str] = []
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

    def send_command(self, command: str) -> None:
        """A JSON command on the panel's MAC command topic, as the web UI's buttons send ("firmware_update", ...)."""
        self.broker.publish("nspanel/%s/command" % self.panel_mac, json.dumps({"command": command}))

    def publish_online(self, online: bool = True) -> None:
        self.broker.publish(self.base + "/status/status", "online" if online else "offline", retain=True)

    def publish_config(self, config: Optional["pb.NSPanelConfig"] = None) -> None:
        if config is not None:
            self.config = config
        self.broker.publish(self.config_topic(), self.config_bytes(), retain=True)

    def config_bytes(self) -> bytes:
        """The config as published: with the LVGL look, whether or not the pinned schema has its fields."""
        config = pb.NSPanelConfig()
        config.CopyFrom(self.config)
        extra = b""
        if self.lvgl_look is not None:
            fields = config.DESCRIPTOR.fields_by_name
            for name, number in (("lvgl_theme", 49), ("lvgl_background", 50)):
                value = self.lvgl_look[name.split("_", 1)[1]]
                if name in fields:
                    setattr(config, name, value)
                else:
                    extra += _string_field(number, value)
        return config.SerializeToString() + extra

    def publish_room(self, room_id: int) -> None:
        self.broker.publish("%s/room/%d/state" % (self.base, room_id), self.rooms[room_id].status_bytes(), retain=True)

    def publish_all_rooms(self) -> None:
        rooms = list(self.rooms.values())
        merged = Room(0, "All rooms",
                      num_ceiling_lights=sum(r.num_ceiling_lights for r in rooms),
                      num_table_lights=sum(r.num_table_lights for r in rooms),
                      num_ceiling_lights_on=sum(r.num_ceiling_lights_on for r in rooms),
                      num_table_lights_on=sum(r.num_table_lights_on for r in rooms))
        if all(r.num_color_temperature_lights is not None for r in rooms):
            merged.num_color_temperature_lights = sum(r.num_color_temperature_lights for r in rooms)
        self.broker.publish(self.base + "/all_rooms_status", merged.status_bytes(), retain=True)

    def publish_entity_page(self, page_id: int) -> None:
        self.broker.publish("%s/entity_pages/%d/state" % (self.base, page_id), self.entity_pages[page_id].SerializeToString(), retain=True)

    def publish_weather(self, update: Optional["pb.NSPanelWeatherUpdate"] = None) -> "pb.NSPanelWeatherUpdate":
        """Weather, as MQTTManagerWeather sends it (retained). Returns what was sent."""
        if update is None:
            update = default_weather()
        self.broker.publish(self.base + "/status/weather", update.SerializeToString(), retain=True)
        return update

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

    def wait_for_look_report(self, since: int = 0, timeout: float = 30) -> dict:
        with self._cond:
            if not wait_for(self._cond, lambda: len(self.look_reports) > since, timeout, self.abort_check):
                raise TimeoutError("panel reported no look within %.0fs" % timeout)
            return self.look_reports[since]

    def look_mark(self) -> int:
        with self._cond:
            return len(self.look_reports)

    def command_mark(self) -> int:
        with self._cond:
            return len(self.commands)

    def registration_mark(self) -> int:
        with self._cond:
            return len(self.register_requests)

    # ---- internals ---------------------------------------------------------

    def _on_message(self, msg: Message) -> None:
        # Broker listeners only see what the panel published, never the harness's own messages.
        if msg.topic == "nspanel/mqttmanager/command":
            self._on_register_request(msg)
        elif msg.topic == self.command_topic:
            cmd = pb.NSPanelMQTTManagerCommand()
            try:
                cmd.ParseFromString(msg.payload)
                report = lvgl_look_of(msg.payload)
            except Exception:
                log.warning("undecodable command on %s: %r", msg.topic, msg.payload[:64])
                return
            if report is not None:
                log.info("look from panel: %s", report)
                with self._cond:
                    self.look_reports.append(report)
                    self._cond.notify_all()
                if self.confirm_looks:
                    self.lvgl_look = {"theme": report["theme"], "background": report["background"]}
                    self.publish_config()
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
        commands, self.commands_after_accept = self.commands_after_accept, []
        for command in commands:
            self.send_command(command)
