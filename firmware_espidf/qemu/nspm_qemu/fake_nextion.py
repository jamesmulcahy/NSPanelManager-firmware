"""A stand-in for the Nextion display, attached to the emulated ESP32's UART2.

The firmware drives the display with ASCII instructions terminated by
0xFF 0xFF 0xFF ("page home", "home.room.txt=\"Kitchen\"", "vis b_x,0", ...).
This fake parses them into a model of what the screen would show, answers the
few instructions that expect a reply (`connect`, `get <x>.val`), and lets
tests inject what a user would do: touches, slider moves, sleep and wake.

Component keys in the model are always fully qualified, `page.component.attr`.
An unqualified assignment (`b_scenes.pic=59`) is attributed to the page last
selected with `page <name>`, which is how the real display resolves it.
"""

import logging
import re
import socket
import struct
import threading
import time
from typing import Callable, Dict, List, Optional, Union

from .waiting import wait_for

log = logging.getLogger("nextion")

TERMINATOR = b"\xff\xff\xff"

# Return codes the firmware understands (lib/Nextion/Nextion_constans.hpp).
TOUCH_EVENT = 0x65
NUMBER_RESPONSE = 0x71
SLEEP = 0x92
WAKE = 0x93
STRING_DATA = 0x99

COMOK = b"comok 2,30614-0,NX4832T035_011R,163,61488,D264B8204F0E1828,16777216"

_ASSIGN = re.compile(r"^([A-Za-z_][\w.\[\]]*)=(.*)$", re.S)

Value = Union[int, str]


class FakeNextion:
    def __init__(self):
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind(("127.0.0.1", 0))
        self._server.listen(1)
        self.port = self._server.getsockname()[1]
        self._sock: Optional[socket.socket] = None
        self._write_lock = threading.Lock()
        self._cond = threading.Condition()

        self.page: Optional[str] = None
        self.page_history: List[str] = []
        self.components: Dict[str, Value] = {}
        self.commands: List[str] = []  # every instruction received, in order
        self.system: Dict[str, Value] = {}  # dim, sleep, bkcmd, thsp, ...
        # Called while waiting; raises to abort the wait (Rig sets QemuPanel.check_alive).
        self.abort_check: Optional[Callable[[], None]] = None

        # Component ids are per page on a real display and the firmware only
        # ever reads them back as numbers, so tests name pages by name and the
        # fake hands out stable small integers for the page byte.
        self._page_ids: Dict[str, int] = {}

        self._thread = threading.Thread(target=self._run, name="fake-nextion", daemon=True)
        self._thread.start()

    # ---- user input --------------------------------------------------------

    def touch(self, component_id: int, pressed: bool, page: Optional[str] = None) -> None:
        """Send a 0x65 touch event, as the display does when a component is pressed or released."""
        page_id = self._page_id(page or self.page or "")
        self._send(bytes([TOUCH_EVENT, page_id, component_id, 1 if pressed else 0]))

    def press(self, component_id: int, hold: float = 0.1, page: Optional[str] = None) -> None:
        """A tap: press, hold briefly, release."""
        self.touch(component_id, True, page)
        time.sleep(hold)
        self.touch(component_id, False, page)

    def long_press(self, component_id: int, hold: float, page: Optional[str] = None) -> None:
        self.press(component_id, hold, page)

    def set_value(self, name: str, value: int) -> None:
        """Set what a component reports for `get <name>.val`, e.g. after a slider drag.

        `name` is as the firmware asks for it, e.g. "home.s_brightness".
        """
        with self._cond:
            self.components[self._qualify(name + ".val")] = value

    def slide(self, name: str, component_id: int, value: int, page: Optional[str] = None) -> None:
        """Move a slider: update its value, then send the release event the HMI emits."""
        self.set_value(name, value)
        self.touch(component_id, False, page)

    def send_string(self, data: Union[bytes, str]) -> None:
        """Send a 0x99 string event (the TFT's printh-style custom data)."""
        if isinstance(data, str):
            data = data.encode()
        self._send(bytes([STRING_DATA]) + data)

    def sleep(self) -> None:
        self._send(bytes([SLEEP]))

    def wake(self) -> None:
        self._send(bytes([WAKE]))

    def boot(self) -> None:
        """Emit the 'NSPM' flag the NSPanel Manager TFT prints when it starts."""
        self._send(b"NSPM")

    # ---- observation -------------------------------------------------------

    def get(self, key: str, default: Optional[Value] = None) -> Optional[Value]:
        with self._cond:
            return self.components.get(self._qualify(key), default)

    def wait_for(self, predicate: Callable[["FakeNextion"], bool], timeout: float = 30, what: str = "condition") -> None:
        with self._cond:
            if not wait_for(self._cond, lambda: predicate(self), timeout, self.abort_check):
                raise TimeoutError("display: %s not met within %.0fs (page=%r)" % (what, timeout, self.page))

    def wait_for_page(self, page: str, timeout: float = 30) -> None:
        self.wait_for(lambda n: n.page == page, timeout, "page %r" % page)

    def wait_for_value(self, key: str, expected: Value, timeout: float = 30) -> None:
        key = self._qualify(key)
        self.wait_for(lambda n: n.components.get(key) == expected, timeout, "%s == %r (is %r)" % (key, expected, self.components.get(key)))

    def wait_for_command(self, pattern: str, timeout: float = 30, since: int = 0) -> str:
        rx = re.compile(pattern)
        found: List[str] = []

        def match(n):
            for c in n.commands[since:]:
                if rx.search(c):
                    found.append(c)
                    return True
            return False

        self.wait_for(match, timeout, "command matching %r" % pattern)
        return found[0]

    def mark(self) -> int:
        with self._cond:
            return len(self.commands)

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def close(self) -> None:
        for s in (self._sock, self._server):
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass

    # ---- internals ---------------------------------------------------------

    def _page_id(self, page: str) -> int:
        return self._page_ids.setdefault(page, len(self._page_ids))

    def _qualify(self, key: str) -> str:
        if key.count(".") >= 2 or self.page is None:
            return key
        return "%s.%s" % (self.page, key)

    def _send(self, frame: bytes) -> None:
        if self._sock is None:
            raise RuntimeError("panel UART is not connected")
        with self._write_lock:
            self._sock.sendall(frame + TERMINATOR)

    def _run(self) -> None:
        try:
            self._sock, _ = self._server.accept()
        except OSError:
            return
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        buf = b""
        while True:
            try:
                data = self._sock.recv(4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while TERMINATOR in buf:
                raw, buf = buf.split(TERMINATOR, 1)
                self._handle(raw)

    def _handle(self, raw: bytes) -> None:
        cmd = raw.decode("utf-8", errors="replace")
        with self._cond:
            self.commands.append(cmd)
            try:
                reply = self._apply(cmd)
            except Exception:
                log.exception("failed to interpret %r", cmd)
                reply = None
            self._cond.notify_all()
        if reply is not None:
            self._send(reply)

    def _apply(self, cmd: str) -> Optional[bytes]:
        log.debug("<- %s", cmd)
        if cmd == "connect":
            return COMOK
        if cmd.startswith("page "):
            self.page = cmd[5:].strip()
            self.page_history.append(self.page)
            return None
        if cmd.startswith("get "):
            key = self._qualify(cmd[4:].strip())
            value = self.components.get(key, 0)
            if not isinstance(value, int):
                log.warning("get %s: non-numeric value %r, answering 0", key, value)
                value = 0
            return bytes([NUMBER_RESPONSE]) + struct.pack("<i", value)
        if cmd.startswith("vis "):
            name, _, state = cmd[4:].partition(",")
            self.components[self._qualify(name.strip() + ".vis")] = int(state.strip() or 0)
            return None
        m = _ASSIGN.match(cmd)
        if m:
            key, value = m.group(1), self._parse_value(m.group(2))
            if "." in key:
                self.components[self._qualify(key)] = value
            else:
                self.system[key] = value
            return None
        # Anything else (fill, rest, the init jibberish, ...) is just recorded.
        return None

    @staticmethod
    def _parse_value(text: str) -> Value:
        text = text.strip()
        if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
            return text[1:-1].replace('\\"', '"').replace("\\r", "\r")
        try:
            return int(text)
        except ValueError:
            return text
