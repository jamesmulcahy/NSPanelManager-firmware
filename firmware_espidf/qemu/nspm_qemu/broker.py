"""A throwaway mosquitto per test, plus the hooks tests need around it.

    panel ──► FaultProxy ──► mosquitto ◄── observer (paho, MQTT 5)

- mosquitto is the real broker, so retained messages, last wills and QoS
  behave exactly as they do in the house.
- The observer subscribes to `#` and records every message. It is also the
  client the harness publishes with. *No Local* keeps it from hearing its own
  publishes, so the record can tell harness messages from everything else, and
  *Retain As Published* keeps retain flags visible.
- The panel connects through FaultProxy, a TCP relay, so `drop_client()` cuts
  the connection the way a network fault would. mosquitto then sees an unclean
  disconnect and publishes the panel's will.
- Connects, subscribes and disconnects are read from mosquitto's own log
  (`log_type subscribe` / `unsubscribe` / `notice`).
"""

import logging
import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Set

import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion
from paho.mqtt.subscribeoptions import SubscribeOptions

from .waiting import wait_for

log = logging.getLogger("broker")

HARNESS = "<test>"

# mosquitto 1.6 and 2.0 log formats (2.0 adds ":port" after the address).
_CONNECT = re.compile(r"^New client connected from \S+ as (\S+) \(")
_DISCONNECT = re.compile(r"^(?:Client (\S+) (?:disconnected|closed its connection)|Socket error on client (\S+), disconnecting)")
# Topics may contain spaces (e.g. a panel named "Master Bedroom"). Only lines whose
# first word is a connected client id are treated as (un)subscribes.
_SUBSCRIBE = re.compile(r"^(\S+) (\d) (.+)$")
_UNSUBSCRIBE = re.compile(r"^(\S+) (.+)$")


@dataclass
class Message:
    topic: str
    payload: bytes
    retain: bool
    sender: str  # HARNESS for messages the harness published, "" for anything else seen on the broker
    time: float = field(default_factory=time.monotonic)


def topic_matches(pattern: str, topic: str) -> bool:
    return mqtt.topic_matches_sub(pattern, topic)


def find_mosquitto() -> str:
    for c in (os.environ.get("MOSQUITTO"), shutil.which("mosquitto"), "/usr/sbin/mosquitto"):
        if c and os.path.exists(c):
            return c
    raise FileNotFoundError("mosquitto not found; install it (apt install mosquitto) or set MOSQUITTO")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _connect_client_id(data: bytes) -> Optional[str]:
    """Client id from the first packet of a stream if it is a complete CONNECT, else None."""
    if len(data) < 2 or data[0] >> 4 != 1:
        return None
    pos, length, shift = 1, 0, 0
    while True:
        if pos >= len(data):
            return None
        byte = data[pos]
        length |= (byte & 0x7F) << shift
        pos += 1
        shift += 7
        if not byte & 0x80:
            break
    if len(data) < pos + length:
        return None
    pos += 2 + int.from_bytes(data[pos:pos + 2], "big")  # protocol name
    level = data[pos]
    pos += 4  # level, flags, keepalive
    if level == 5:  # MQTT 5: skip the properties block
        prop_len, shift = 0, 0
        while True:
            byte = data[pos]
            prop_len |= (byte & 0x7F) << shift
            pos += 1
            shift += 7
            if not byte & 0x80:
                break
        pos += prop_len
    id_len = int.from_bytes(data[pos:pos + 2], "big")
    return data[pos + 2:pos + 2 + id_len].decode(errors="replace")


class FaultProxy:
    """TCP relay in front of the broker whose connections can be cut by MQTT client id."""

    def __init__(self, upstream_port: int):
        self.upstream_port = upstream_port
        self._server = socket.create_server(("127.0.0.1", 0))
        self.port = self._server.getsockname()[1]
        self._lock = threading.Lock()
        self._conns: List[dict] = []
        threading.Thread(target=self._accept_loop, name="mqtt-proxy", daemon=True).start()

    def drop(self, client_prefix: str) -> int:
        with self._lock:
            victims = [c for c in self._conns if c["client_id"] and c["client_id"].startswith(client_prefix)]
        for c in victims:
            self._close(c)
        return len(victims)

    def close(self) -> None:
        try:
            self._server.close()
        except OSError:
            pass
        with self._lock:
            conns = list(self._conns)
        for c in conns:
            self._close(c)

    def _close(self, conn: dict) -> None:
        for s in (conn["down"], conn["up"]):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()
        with self._lock:
            if conn in self._conns:
                self._conns.remove(conn)

    def _accept_loop(self) -> None:
        while True:
            try:
                down, _ = self._server.accept()
            except OSError:
                return
            try:
                up = socket.create_connection(("127.0.0.1", self.upstream_port))
            except OSError:
                down.close()
                continue
            conn = {"down": down, "up": up, "client_id": None}
            with self._lock:
                self._conns.append(conn)
            threading.Thread(target=self._pump, args=(conn, down, up, True), daemon=True).start()
            threading.Thread(target=self._pump, args=(conn, up, down, False), daemon=True).start()

    def _pump(self, conn: dict, src: socket.socket, dst: socket.socket, from_client: bool) -> None:
        head = b""
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                if from_client and conn["client_id"] is None:
                    head += data
                    conn["client_id"] = _connect_client_id(head)
                dst.sendall(data)
        except OSError:
            pass
        self._close(conn)


class Broker:
    """mosquitto on 127.0.0.1. The panel uses `panel_port` (the proxy); QEMU reaches it as 10.0.2.2."""

    def __init__(self, log_path: Optional[str] = None):
        self._cond = threading.Condition()
        self.messages: List[Message] = []
        # (kind, client_id, topic) for connect / subscribe / unsubscribe / disconnect
        self.events: List[tuple] = []
        self._subs: Dict[str, Set[str]] = {}
        self._connected: Set[str] = set()
        self._listeners: List[Callable[[Message], None]] = []
        # Called while waiting; raises to abort the wait (Rig sets QemuPanel.check_alive).
        self.abort_check: Optional[Callable[[], None]] = None

        self._tmp = tempfile.mkdtemp(prefix="nspm-mosquitto-")
        self.port = _free_port()
        conf = os.path.join(self._tmp, "mosquitto.conf")
        with open(conf, "w") as f:
            f.write("\n".join([
                "listener %d 127.0.0.1" % self.port,
                "allow_anonymous true",
                "persistence false",
                "log_dest stdout",
                "log_timestamp false",
                "log_type error",
                "log_type warning",
                "log_type notice",
                "log_type subscribe",
                "log_type unsubscribe",
                "",
            ]))
        self._log_file = open(log_path, "w") if log_path else None
        self._proc = subprocess.Popen([find_mosquitto(), "-c", conf], stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=self._read_log, name="mosquitto-log", daemon=True).start()
        self._wait_listening()

        self.proxy = FaultProxy(self.port)
        self.panel_port = self.proxy.port

        self._subscribed = threading.Event()
        self._client = mqtt.Client(CallbackAPIVersion.VERSION2, client_id="nspm-test-observer",
                                   protocol=mqtt.MQTTv5)
        self._client.on_message = self._on_message
        self._client.on_subscribe = lambda *a: self._subscribed.set()
        self._client.connect("127.0.0.1", self.port)
        self._client.subscribe("#", options=SubscribeOptions(qos=0, noLocal=True, retainAsPublished=True))
        self._client.loop_start()
        if not self._subscribed.wait(10):
            raise RuntimeError("observer could not subscribe to mosquitto")

    # ---- harness API -------------------------------------------------------

    def publish(self, topic: str, payload, retain: bool = False, qos: int = 0) -> None:
        if isinstance(payload, str):
            payload = payload.encode()
        payload = bytes(payload)
        with self._cond:
            self.messages.append(Message(topic, payload, retain, HARNESS))
            self._cond.notify_all()
        self._client.publish(topic, payload, qos=qos, retain=retain)

    def add_listener(self, fn: Callable[[Message], None]) -> None:
        """Call fn for every message seen on the broker that the harness did not publish."""
        self._listeners.append(fn)

    def wait_for_message(self, topic_filter: str, predicate: Callable[[Message], bool] = lambda m: True,
                         timeout: float = 30, since: int = 0) -> Message:
        """Wait for a message not published by the harness, after index `since`."""
        found: List[Message] = []

        def match():
            for msg in self.messages[since:]:
                if msg.sender != HARNESS and topic_matches(topic_filter, msg.topic) and predicate(msg):
                    found.append(msg)
                    return True
            return False

        with self._cond:
            if not wait_for(self._cond, match, timeout, self.abort_check):
                raise TimeoutError("no message on %r within %.0fs" % (topic_filter, timeout))
        return found[0]

    def wait_for_subscription(self, client_prefix: str, topic: str, timeout: float = 30, since: int = 0) -> None:
        self.wait_for_event("subscribe", client_prefix, topic, timeout, since)

    def wait_for_event(self, kind: str, client_prefix: str, topic: Optional[str] = None,
                       timeout: float = 30, since: int = 0) -> None:
        def match():
            return any(k == kind and cid.startswith(client_prefix) and (topic is None or t == topic)
                       for k, cid, t in self.events[since:])

        with self._cond:
            if not wait_for(self._cond, match, timeout, self.abort_check):
                raise TimeoutError("no %s by %s%s within %.0fs" % (kind, client_prefix, " on %r" % topic if topic else "", timeout))

    def subscriptions(self, client_prefix: str) -> List[str]:
        """Current subscriptions of a connected client, rebuilt from mosquitto's log."""
        with self._cond:
            for cid in self._connected:
                if cid.startswith(client_prefix):
                    return sorted(self._subs.get(cid, ()))
        return []

    def is_connected(self, client_prefix: str) -> bool:
        with self._cond:
            return any(cid.startswith(client_prefix) for cid in self._connected)

    def drop_client(self, client_prefix: str) -> int:
        """Cut matching panel connections at the proxy, as a network fault would. Returns the count."""
        return self.proxy.drop(client_prefix)

    def mark(self) -> int:
        """Index to pass as `since` to only see messages from now on."""
        with self._cond:
            return len(self.messages)

    def event_mark(self) -> int:
        with self._cond:
            return len(self.events)

    def close(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()
        self.proxy.close()
        self._proc.terminate()
        try:
            self._proc.wait(5)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait()
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None
        shutil.rmtree(self._tmp, ignore_errors=True)

    # ---- internals ---------------------------------------------------------

    def _wait_listening(self) -> None:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self._proc.poll() is not None:
                raise RuntimeError("mosquitto exited with %s" % self._proc.returncode)
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("mosquitto did not start listening on %d" % self.port)

    def _on_message(self, client, userdata, msg) -> None:
        message = Message(msg.topic, bytes(msg.payload), bool(msg.retain), "")
        with self._cond:
            self.messages.append(message)
            self._cond.notify_all()
        for fn in list(self._listeners):
            try:
                fn(message)
            except Exception:  # A broken listener must not stop delivery to the others.
                log.exception("listener failed on %s", message.topic)

    def _read_log(self) -> None:
        assert self._proc.stdout is not None
        for raw in self._proc.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip()
            if self._log_file is not None:
                self._log_file.write(line + "\n")
                self._log_file.flush()
            self._parse_log(line)

    def _record(self, kind: str, client_id: str, topic: Optional[str]) -> None:
        self.events.append((kind, client_id, topic))
        self._cond.notify_all()

    def _parse_log(self, line: str) -> None:
        with self._cond:
            m = _CONNECT.match(line)
            if m:
                cid = m.group(1)
                self._connected.add(cid)
                self._subs[cid] = set()  # every client here uses a clean session
                self._record("connect", cid, None)
                return
            m = _DISCONNECT.match(line)
            if m:
                cid = m.group(1) or m.group(2)
                self._connected.discard(cid)
                self._record("disconnect", cid, None)
                return
            m = _SUBSCRIBE.match(line)
            if m and m.group(1) in self._connected:
                self._subs.setdefault(m.group(1), set()).add(m.group(3))
                self._record("subscribe", m.group(1), m.group(3))
                return
            m = _UNSUBSCRIBE.match(line)
            if m and m.group(1) in self._connected:
                self._subs.get(m.group(1), set()).discard(m.group(2))
                self._record("unsubscribe", m.group(1), m.group(2))
