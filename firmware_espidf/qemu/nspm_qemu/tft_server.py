"""The manager's HTTP side of a GUI update, with hooks to break it.

The panel fetches the TFT from NSPanelManager's Django views
(docker/web/nspanelmanager/web/views.py):

  GET /download_tft/<id>         the whole file, or with `Range: bytes=a-b`
                                 the slice data[a:b] (exclusive end) with
                                 status 200, never 206
  GET /checksum_tft_file/<id>    md5 hex digest of the same file, no newline

TftServer reproduces exactly that, quirks included, so the firmware sees what
it sees in the house. Every request is recorded, and faults are armed against
an absolute offset in the file ("cut the connection when the stream reaches
byte 150000"), which is how a network fault lands mid-download.
"""

import hashlib
import logging
import re
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable, List, Optional, Tuple

from .waiting import wait_for

log = logging.getLogger("tft-server")

_DOWNLOAD = re.compile(r"^/download_tft/(\d+)$")
_CHECKSUM = re.compile(r"^/checksum_tft_file/(\d+)$")

# How much of the body is written per send(). Small enough that a fault armed
# at an offset lands close to it, large enough not to slow the download.
SEGMENT = 1460


@dataclass
class Request:
    path: str
    range: Optional[Tuple[int, int]]  # as the panel asked, end exclusive like the server reads it
    status: int = 0
    start: int = 0  # file offset of the first body byte
    sent: int = 0  # body bytes written before the response ended
    length: int = 0  # Content-Length announced (-1 when none)
    outcome: str = "open"  # "complete", "reset", "fin", "client-closed", "refused", "injected"
    time: float = field(default_factory=time.monotonic)

    @property
    def end(self) -> int:
        return self.start + self.sent


@dataclass
class _OffsetFault:
    kind: str  # "reset", "fin", "stall", "swap"
    offset: int
    seconds: float = 0.0
    data: bytes = b""


class TftServer:
    """Serves one TFT image to one panel on 127.0.0.1; QEMU reaches it as 10.0.2.2:<port>."""

    def __init__(self, data: bytes = b""):
        self._lock = threading.Condition()
        self._data = data
        self.requests: List[Request] = []
        self.abort_check: Optional[Callable[[], None]] = None

        self._offset_faults: List[_OffsetFault] = []
        self._drop_next = 0
        self._respond_next: List[Tuple[int, bytes]] = []
        self._ignore_range_next = 0
        self._no_length_next = 0
        self._segment = SEGMENT
        self._segment_delay = 0.0

        server = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"  # the panel asks for keep-alive

            def do_GET(self):
                server._handle(self)

            def log_message(self, fmt, *args):
                log.debug("%s " + fmt, self.client_address[0], *args)

        class Server(ThreadingHTTPServer):
            def handle_error(self, request, client_address):
                # The panel drops keep-alive connections, and faults reset them on purpose.
                log.debug("connection from %s ended with an error", client_address, exc_info=True)

        self._httpd = Server(("127.0.0.1", 0), Handler)
        self._httpd.daemon_threads = True
        self.port = self._httpd.server_address[1]
        threading.Thread(target=self._httpd.serve_forever, name="tft-server", daemon=True).start()

    # ---- the file ----------------------------------------------------------

    @property
    def data(self) -> bytes:
        with self._lock:
            return self._data

    @data.setter
    def data(self, value: bytes) -> None:
        with self._lock:
            self._data = value

    @property
    def md5(self) -> str:
        return hashlib.md5(self.data).hexdigest()

    # ---- faults ------------------------------------------------------------
    # Offset faults fire once, the first time a ranged download's body reaches
    # `offset` (an absolute position in the file). The panel's first GET has no
    # Range and only reads the headers (to learn the size), so it is never the
    # target. Request faults apply to the next n download requests, whatever
    # their range.

    def reset_at(self, offset: int) -> None:
        """Abort the connection with a TCP RST once the body reaches `offset`."""
        self._arm(_OffsetFault("reset", offset))

    def fin_at(self, offset: int) -> None:
        """Close the connection cleanly at `offset`, short of the announced Content-Length."""
        self._arm(_OffsetFault("fin", offset))

    def stall_at(self, offset: int, seconds: float) -> None:
        """Stop sending at `offset` for `seconds`, keeping the connection open, then carry on."""
        self._arm(_OffsetFault("stall", offset, seconds=seconds))

    def swap_file_at(self, offset: int, data: bytes) -> None:
        """Replace the file once the stream reaches `offset`, as an upload in the web UI would.

        Django reads the file per request, so the response in progress carries on
        with the old bytes; later requests (resumes, the checksum) see the new one.
        """
        self._arm(_OffsetFault("swap", offset, data=data))

    def trickle(self, segment: int, delay: float) -> None:
        """Write the body `segment` bytes at a time with `delay` between, forcing partial reads."""
        with self._lock:
            self._segment, self._segment_delay = segment, delay

    def drop_next(self, n: int) -> None:
        """Reset the next n download connections before answering, like a manager that is down."""
        with self._lock:
            self._drop_next += n

    def respond_next(self, n: int, status: int, body: bytes) -> None:
        """Answer the next n downloads with `status` and `body` instead of the file."""
        with self._lock:
            self._respond_next += [(status, body)] * n

    def ignore_range_next(self, n: int) -> None:
        """Answer the next n downloads with the whole file, whatever Range asked for."""
        with self._lock:
            self._ignore_range_next += n

    def no_content_length_next(self, n: int) -> None:
        """Answer the next n downloads without a Content-Length (closing to end the body)."""
        with self._lock:
            self._no_length_next += n

    def _arm(self, fault: _OffsetFault) -> None:
        with self._lock:
            self._offset_faults.append(fault)
            self._offset_faults.sort(key=lambda f: f.offset)

    # ---- observation -------------------------------------------------------

    def downloads(self) -> List[Request]:
        with self._lock:
            return [r for r in self.requests if r.path.startswith("/download_tft/")]

    def wait_for_request(self, predicate: Callable[[Request], bool], timeout: float = 60,
                         what: str = "request", since: int = 0) -> Request:
        found: List[Request] = []

        def match():
            for r in self.requests[since:]:
                if predicate(r):
                    found.append(r)
                    return True
            return False

        with self._lock:
            if not wait_for(self._lock, match, timeout, self.abort_check):
                raise TimeoutError("tft server: %s not seen within %.0fs" % (what, timeout))
        return found[0]

    def mark(self) -> int:
        with self._lock:
            return len(self.requests)

    def close(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()

    # ---- serving -----------------------------------------------------------

    def _handle(self, h: BaseHTTPRequestHandler) -> None:
        rng = None
        header = h.headers.get("Range")
        if header and header.startswith("bytes="):
            # Exactly as views.download_tft parses it.
            parts = header[6:].split("-")
            rng = (int(parts[0]), int(parts[1]))
        req = Request(h.path, rng)
        with self._lock:
            self.requests.append(req)
            self._lock.notify_all()
        log.info("GET %s Range=%s", h.path, header)

        if _CHECKSUM.match(h.path):
            self._send_simple(h, req, 200, self.md5.encode())
        elif _DOWNLOAD.match(h.path):
            self._serve_download(h, req)
        else:
            self._send_simple(h, req, 404, b"Not found")

    def _send_simple(self, h: BaseHTTPRequestHandler, req: Request, status: int, body: bytes) -> None:
        h.send_response(status)
        h.send_header("Content-Type", "text/html" if status != 200 else "application/octet-stream")
        h.send_header("Content-Length", str(len(body)))
        h.end_headers()
        h.wfile.write(body)
        h.wfile.flush()
        self._finish(req, status, len(body), len(body), "complete" if status == 200 else "injected")

    def _serve_download(self, h: BaseHTTPRequestHandler, req: Request) -> None:
        with self._lock:
            data = self._data
            if self._drop_next:
                self._drop_next -= 1
                drop = True
            else:
                drop = False
            injected = self._respond_next.pop(0) if self._respond_next else None
            ignore_range = self._ignore_range_next > 0
            if ignore_range:
                self._ignore_range_next -= 1
            no_length = self._no_length_next > 0
            if no_length:
                self._no_length_next -= 1
            segment, delay = self._segment, self._segment_delay

        if drop:
            self._reset(h)
            self._finish(req, 0, 0, 0, "refused")
            return
        if injected is not None:
            status, body = injected
            self._send_simple(h, req, status, body)
            return

        start, end = 0, len(data)
        if req.range is not None and not ignore_range:
            start, end = req.range
        body = data[start:end]
        start = min(start, len(data))  # what data[start:end] actually begins at
        req.start = start

        h.send_response(200)
        h.send_header("Content-Type", "application/octet-stream")
        if no_length:
            h.send_header("Connection", "close")
            req.length = -1
        else:
            h.send_header("Content-Length", str(len(body)))
            req.length = len(body)
        h.end_headers()

        pos = 0
        try:
            while pos < len(body):
                fault = self._next_fault(start + pos, start + len(body)) if req.range is not None else None
                limit = len(body) if fault is None else fault.offset - start
                n = min(segment, limit - pos)
                if n > 0:
                    h.wfile.write(body[pos:pos + n])
                    h.wfile.flush()
                    pos += n
                    req.sent = pos
                    if delay:
                        time.sleep(delay)
                    continue
                # Reached an armed fault's offset.
                self._consume(fault)
                log.info("fault %s at offset %d", fault.kind, fault.offset)
                if fault.kind == "reset":
                    self._reset(h)
                    self._finish(req, 200, req.length, pos, "reset")
                    return
                if fault.kind == "fin":
                    h.close_connection = True
                    self._fin(h)
                    self._finish(req, 200, req.length, pos, "fin")
                    return
                if fault.kind == "stall":
                    time.sleep(fault.seconds)
                elif fault.kind == "swap":
                    self.data = fault.data
        except (BrokenPipeError, ConnectionResetError):
            self._finish(req, 200, req.length, pos, "client-closed")
            h.close_connection = True
            return
        if no_length:
            h.close_connection = True
        self._finish(req, 200, req.length, pos, "complete")

    def _next_fault(self, pos: int, end: int) -> Optional[_OffsetFault]:
        with self._lock:
            for f in self._offset_faults:
                if pos <= f.offset < end:
                    return f
        return None

    def _consume(self, fault: _OffsetFault) -> None:
        with self._lock:
            if fault in self._offset_faults:
                self._offset_faults.remove(fault)

    def _finish(self, req: Request, status: int, length: int, sent: int, outcome: str) -> None:
        with self._lock:
            req.status, req.length, req.sent, req.outcome = status, length, sent, outcome
            self._lock.notify_all()

    @staticmethod
    def _reset(h: BaseHTTPRequestHandler) -> None:
        sock = h.connection
        try:
            h.wfile.flush()
        except OSError:
            pass
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        h.close_connection = True
        sock.close()

    @staticmethod
    def _fin(h: BaseHTTPRequestHandler) -> None:
        try:
            h.wfile.flush()
            h.connection.shutdown(socket.SHUT_WR)
        except OSError:
            pass
