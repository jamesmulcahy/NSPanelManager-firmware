"""Fake service: the manager's album art, as the panel's LvglArt fetches it.

NSPanelManager's image server answers GET /nextion-img/media_player/<id>/album_art?v=<hash>&w=&h=
with the art cropped and scaled to w x h as RGB565 (w * h * 2 bytes). ArtServer serves a flat colour
chosen by `v`, at whatever size is asked for, and records every request so a test can count the
fetches the panel made.
"""

import hashlib
import logging
import re
import struct
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, List, Optional
from urllib.parse import parse_qs, urlparse

log = logging.getLogger("art-server")

_PATH = re.compile(r"^/nextion-img/media_player/(\d+)/album_art$")


def rgb565(version: str) -> int:
    """A colour for a track's art, the same for every request of the same `v`."""
    digest = hashlib.md5(version.encode()).digest()
    return ((digest[0] & 0xF8) << 8) | ((digest[1] & 0xFC) << 3) | (digest[2] >> 3)


class ArtServer:
    """Serves album art on 127.0.0.1; QEMU reaches it as 10.0.2.2:<port>."""

    def __init__(self):
        self.requests: List[Dict[str, str]] = []
        self._lock = threading.Lock()
        server = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                server._handle(self)

            def log_message(self, fmt, *args):
                log.debug("%s " + fmt, self.client_address[0], *args)

        class Server(ThreadingHTTPServer):
            def handle_error(self, request, client_address):
                log.debug("connection from %s ended with an error", client_address, exc_info=True)

        self._httpd = Server(("127.0.0.1", 0), Handler)
        self._httpd.daemon_threads = True
        self.port = self._httpd.server_address[1]
        threading.Thread(target=self._httpd.serve_forever, name="art-server", daemon=True).start()

    def url(self, player_id: int, version: str) -> str:
        """The album_art_url a manager puts in a player's state: absolute, with ?v=<hash of the source>."""
        return "http://10.0.2.2:%d/nextion-img/media_player/%d/album_art?v=%s" % (self.port, player_id, version)

    def requested(self, version: Optional[str] = None) -> List[Dict[str, str]]:
        with self._lock:
            return [r for r in self.requests if version is None or r["v"] == version]

    def close(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()

    def _handle(self, h: BaseHTTPRequestHandler) -> None:
        parsed = urlparse(h.path)
        query = {k: v[0] for k, v in parse_qs(parsed.query).items()}
        if not _PATH.match(parsed.path) or "w" not in query or "h" not in query:
            h.send_error(404)
            return
        width, height = int(query["w"]), int(query["h"])
        with self._lock:
            self.requests.append({"path": parsed.path, "v": query.get("v", ""), "w": str(width), "h": str(height)})
        log.info("GET %s v=%s %dx%d", parsed.path, query.get("v"), width, height)
        body = struct.pack("<H", rgb565(query.get("v", ""))) * (width * height)
        h.send_response(200)
        h.send_header("Content-Type", "application/octet-stream")
        h.send_header("Content-Length", str(len(body)))
        h.send_header("X-Image-Width", str(width))
        h.send_header("X-Image-Height", str(height))
        h.end_headers()
        h.wfile.write(body)
