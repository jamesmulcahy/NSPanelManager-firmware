"""A stand-in for the Nextion display, attached to the emulated ESP32's UART2.

The firmware drives the display with ASCII instructions terminated by
0xFF 0xFF 0xFF ("page home", "home.room.txt=\"Kitchen\"", "vis b_x,0", ...).
This fake parses them into a model of what the screen would show, answers the
few instructions that expect a reply (`connect`, `get <x>.val`), and lets
tests inject what a user would do: touches, slider moves, sleep and wake.

It also accepts a GUI upload (`whmi-wri` / `whmi-wris`): from that command on,
the UART carries raw TFT bytes, which the fake writes into an image at its own
cursor and acknowledges per 4096-byte chunk, the way the display does. Tests
compare that image with what the manager served, and can make the display
misbehave (slow, lost or duplicated acks, jumps, stray frames) to see whether
the firmware still ends up with a correct image.

Component keys in the model are always fully qualified, `page.component.attr`.
An unqualified assignment (`b_scenes.pic=59`) is attributed to the page last
selected with `page <name>`, which is how the real display resolves it.
"""

import logging
import re
import socket
import struct
import hashlib
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Set, Tuple, Union

from .waiting import wait_for

log = logging.getLogger("nextion")

TERMINATOR = b"\xff\xff\xff"

# Return codes the firmware understands (lib/Nextion/Nextion_constans.hpp).
TOUCH_EVENT = 0x65
NUMBER_RESPONSE = 0x71
SLEEP = 0x92
WAKE = 0x93
STRING_DATA = 0x99

# Upload protocol replies. They are bare bytes, not terminated by FF FF FF.
UPDATE_ACK = 0x05  # ready for the next chunk
UPDATE_JUMP = 0x08  # v1.2 (whmi-wris): followed by a u32le offset to continue from; 0 = carry on
UPDATE_CHUNK = 4096
FLASH_SIZE = 16777216  # NX4832T035, as reported in COMOK

_UPLOAD = re.compile(r"^whmi-(wris?) (\d+),(\d+),(\d+)$")

COMOK = b"comok 2,30614-0,NX4832T035_011R,163,61488,D264B8204F0E1828,16777216"

_ASSIGN = re.compile(r"^([A-Za-z_][\w.\[\]]*)=(.*)$", re.S)

Value = Union[int, str]


@dataclass
class Upload:
    """One GUI upload as the display saw it."""

    size: int
    baud: int
    protocol: str  # "v1.1" (whmi-wri) or "v1.2" (whmi-wris)
    image: bytearray  # the display's flash after this upload; starts as the previous image
    cursor: int = 0  # where the next received byte lands
    chunk_fill: int = 0  # bytes received since the last ack
    chunks: List[Tuple[int, int, float]] = field(default_factory=list)  # (offset, length, time) per chunk
    acks: List[Tuple[float, bytes]] = field(default_factory=list)  # (time, bytes) sent to the panel
    overrun: int = 0  # bytes received after the image was complete
    complete: bool = False
    abandoned: bool = False  # power-cycled before complete
    started: float = field(default_factory=time.monotonic)

    @property
    def md5(self) -> str:
        return hashlib.md5(bytes(self.image)).hexdigest()

    @property
    def received(self) -> int:
        return sum(length for _, length, _ in self.chunks) + self.chunk_fill


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

        # GUI uploads. `flash` is the TFT the display currently holds (what a
        # v1.2 jump leaves in place). It is "ok" if that TFT runs, or
        # "data_error" when the display would show SYSTEM DATA ERROR: an upload
        # was cut short, or produced an image that is none of `valid_md5s`
        # (the TFTs the manager has served). Empty means every image is valid.
        self.upload: Optional[Upload] = None  # in progress
        self.uploads: List[Upload] = []  # all, including the one in progress
        self.flash: bytes = b""
        self.valid_md5s: Set[str] = set()
        self.display_state = "ok"
        self.power_cycles = 0
        # Ways for the display to misbehave during an upload.
        self.ack_delay = 0.0  # seconds before each ack; ~0.38 at 115200 baud on hardware
        self.jump_offset = 0  # v1.2 reply after the first chunk
        # Send the 0x08 and its offset as two writes. Nextion.cpp's UPDATING path
        # only understands them as separate UART reads (and v1.2 updates work in
        # the field), so that is the default; False sends all five bytes at once.
        self.split_jump = True
        self.reject_upload = False  # never acknowledge whmi-wri(s)
        # Chunk numbers count from 1 within each upload. These fire once and are
        # then forgotten, so a retried upload goes through; the `always_` ones
        # fire on every upload.
        self.drop_ack_at: Set[int] = set()  # chunks not acknowledged
        self.always_drop_ack_at: Set[int] = set()
        self.dup_ack_at: Set[int] = set()  # chunks acknowledged twice
        self.inject_at: Dict[int, bytes] = {}  # chunk -> raw bytes sent right after its ack
        self._pending_replies: List[Tuple[float, bytes]] = []  # (delay, bytes), sent outside the lock

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
        """Emit the 'NSPM' flag the NSPanel Manager TFT prints when it starts.

        A display whose TFT is broken shows SYSTEM DATA ERROR instead and prints nothing.
        """
        if self.display_state != "ok":
            log.info("display boot: %s, no NSPM flag", self.display_state)
            return
        self._send(b"NSPM")

    def power_cycle(self) -> None:
        """The panel switched the display off and on. An upload in progress is lost."""
        with self._cond:
            self.power_cycles += 1
            if self.upload is not None:
                log.warning("power cycle during upload at %d/%d bytes", self.upload.cursor, self.upload.size)
                self.upload.abandoned = True
                self.upload = None
                self.display_state = "data_error"
            self._cond.notify_all()

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
    def last_upload(self) -> Optional[Upload]:
        with self._cond:
            return self.uploads[-1] if self.uploads else None

    def wait_for_upload_start(self, timeout: float = 120, count: int = 1) -> Upload:
        self.wait_for(lambda n: len(n.uploads) >= count, timeout, "upload #%d to start" % count)
        return self.uploads[count - 1]

    def wait_for_upload_progress(self, offset: int, timeout: float = 300) -> None:
        self.wait_for(lambda n: n.upload is not None and n.upload.cursor >= offset, timeout,
                      "upload to reach offset %d" % offset)

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
        self._send_raw(frame + TERMINATOR)

    def _send_raw(self, data: bytes) -> None:
        if self._sock is None:
            raise RuntimeError("panel UART is not connected")
        with self._write_lock:
            self._sock.sendall(data)

    def _run(self) -> None:
        # QEMU connects out to us on every start, including restarts after a firmware reset.
        while True:
            try:
                sock, _ = self._server.accept()
            except OSError:
                return
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._sock = sock
            self._serve(sock)
            if self._sock is sock:
                self._sock = None

    def _serve(self, sock: socket.socket) -> None:
        buf = b""
        while True:
            try:
                data = sock.recv(4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while buf:
                if self.upload is not None:
                    # TFT bytes may contain FF FF FF; nothing is a command until the upload ends.
                    buf = self._receive_upload(buf)
                    continue
                if TERMINATOR not in buf:
                    break
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
        self._flush_upload_replies()

    def _apply(self, cmd: str) -> Optional[bytes]:
        log.debug("<- %s", cmd)
        if cmd == "connect":
            return COMOK
        m = _UPLOAD.match(cmd)
        if m:
            self._start_upload(m)
            return None
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

    # ---- GUI upload ----------------------------------------------------------
    # Called with self._cond held; replies are queued and sent by
    # _flush_upload_replies() once it is released.

    def _start_upload(self, m: "re.Match") -> None:
        size, baud = int(m.group(2)), int(m.group(3))
        protocol = "v1.2" if m.group(1) == "wris" else "v1.1"
        if size > FLASH_SIZE:
            # Recorded, so tests see it, but never acknowledged: the TFT cannot fit.
            log.warning("upload of %d bytes exceeds the display's %d-byte flash; not acknowledged", size, FLASH_SIZE)
            self.uploads.append(Upload(size=size, baud=baud, protocol=protocol, image=bytearray(), abandoned=True))
            return
        image = bytearray(self.flash[:size].ljust(size, b"\0"))
        self.upload = Upload(size=size, baud=baud, protocol=protocol, image=image)
        self.uploads.append(self.upload)
        log.info("upload started: %d bytes at %d baud, %s", size, baud, protocol)
        if self.reject_upload:
            return
        self._queue_reply(bytes([UPDATE_ACK]))

    def _receive_upload(self, buf: bytes) -> bytes:
        """Take TFT bytes from `buf` up to the end of the current chunk; return the rest."""
        with self._cond:
            u = self.upload
            if u is None:
                return buf
            take = min(len(buf), UPDATE_CHUNK - u.chunk_fill, u.size - u.cursor)
            if take <= 0:
                # The panel sent past the end of the image (or past a chunk it was not acked for).
                u.overrun += len(buf)
                self._cond.notify_all()
                return b""
            u.image[u.cursor:u.cursor + take] = buf[:take]
            u.cursor += take
            u.chunk_fill += take
            if u.chunk_fill == UPDATE_CHUNK or u.cursor >= u.size:
                self._end_chunk(u)
            self._cond.notify_all()
        self._flush_upload_replies()
        return buf[take:]

    def _end_chunk(self, u: Upload) -> None:
        start = u.cursor - u.chunk_fill
        u.chunks.append((start, u.chunk_fill, time.monotonic()))
        u.chunk_fill = 0
        n = len(u.chunks)
        if u.cursor >= u.size:
            u.complete = True
            self.upload = None
            self.flash = bytes(u.image)
            ok = not self.valid_md5s or u.md5 in self.valid_md5s
            self.display_state = "ok" if ok else "data_error"
            log.info("upload complete after %d chunks, md5 %s (%s)", n, u.md5, self.display_state)
            return
        if n in self.drop_ack_at or n in self.always_drop_ack_at:
            log.info("dropping ack for chunk %d", n)
            self.drop_ack_at.discard(n)
            return
        if u.protocol == "v1.2" and n == 1:
            jump = self.jump_offset
            if jump:
                u.cursor = jump
            offset = jump.to_bytes(4, "little")
            if self.split_jump:
                self._queue_reply(bytes([UPDATE_JUMP]))
                self._queue_reply(offset, delay=0.05)
            else:
                self._queue_reply(bytes([UPDATE_JUMP]) + offset)
        else:
            self._queue_reply(bytes([UPDATE_ACK]))
        if n in self.dup_ack_at:
            self.dup_ack_at.discard(n)
            self._queue_reply(bytes([UPDATE_ACK]), delay=0.05)
        if n in self.inject_at:
            self._queue_reply(self.inject_at.pop(n), delay=0.05)

    def _queue_reply(self, data: bytes, delay: Optional[float] = None) -> None:
        self._pending_replies.append((self.ack_delay if delay is None else delay, data))

    def _flush_upload_replies(self) -> None:
        with self._cond:
            pending, self._pending_replies = self._pending_replies, []
            u = self.uploads[-1] if self.uploads else None
        for delay, data in pending:
            if delay:
                time.sleep(delay)
            if u is not None:
                with self._cond:
                    u.acks.append((time.monotonic(), data))
            try:
                self._send_raw(data)
            except (OSError, RuntimeError) as e:
                log.warning("could not send upload reply: %s", e)
