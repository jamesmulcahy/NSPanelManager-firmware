"""Library: one emulated panel with its broker, manager and display, started and stopped together.
The tests' `rig` fixture (tests/conftest.py) and web_panel.py are both built on it."""

import json
import os
import time
import urllib.request
from typing import Optional

from . import fake_nextion
from .broker import Broker
from .fake_manager import QEMU_HOST_ADDRESS, FakeManager
from .panel import QemuPanel
from .proto import nspanel as pb
from .stack_manager import Stack, StackManager
from .update_server import UpdateServer

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_BUILD_DIR = os.path.join(PROJECT_DIR, ".pio", "build", "qemu")

# Generous, because CI runners are slow and QEMU runs the guest slower than
# real time. Boot to "registered" takes ~25 s of guest time; most of it is
# esp-mqtt's 10 s reconnect delay after its first attempt beats DHCP.
BOOT_TIMEOUT = 180
# How long a terminal session the boot-time config save ended takes to be restarted: the
# firmware notices within ~2 s (its reverse timeout) and retries 1 s later.
TERMINAL_RESTART_GRACE = 5


class Rig:
    """With `stack`, the panel joins a running NSPanelManager dev stack: its broker and its
    real manager, watched through a StackManager. boot() accepts it in the stack's web UI,
    and stop() deletes it again. Without, it gets its own mosquitto and a FakeManager."""

    def __init__(self, workdir: str, mac: str = "C0:49:EF:00:00:01", hostname: str = "qemu-panel",
                 build_dir: Optional[str] = None, panel_config: Optional[dict] = None,
                 stack: Optional[Stack] = None):
        os.makedirs(workdir, exist_ok=True)
        if stack is None:
            self.broker = Broker(log_path=os.path.join(workdir, "mosquitto.log"))
            # The manager's HTTP side (update downloads), on the port register_accept advertises.
            self.update_server = UpdateServer()
            self.manager = FakeManager(self.broker, port=self.update_server.port)
            if fake_nextion.TERMINAL is not None:
                # LvglRenderer lays out for the region the manager's config names, whatever
                # config.json said at boot: US for the US receiver, EU for the EU one.
                self.manager.config.is_us_panel = fake_nextion.TERMINAL == "us"
        else:
            # The stack's own manager serves updates.
            self.update_server = None
            self.broker = Broker(external=stack.mqtt)
            self.manager = StackManager(self.broker, stack, mac)
        self.stack = stack
        self.hostname = hostname
        if panel_config is None:
            panel_config = self.default_panel_config()
        self.panel = QemuPanel(workdir, build_dir or os.environ.get("NSPM_QEMU_BUILD_DIR", DEFAULT_BUILD_DIR),
                               PROJECT_DIR, mac, panel_config)
        self.nextion = self.panel.nextion
        # Every page a test waits for, and the screen when it fails (tests/conftest.py).
        self.nextion.screenshots_dir = os.path.join(workdir, "screens")
        # Any wait on a fake ends as soon as the firmware panics or QEMU exits.
        for fake in (self.broker, self.manager, self.nextion, self.update_server):
            fake.abort_check = self.panel.check_alive

    def default_panel_config(self) -> dict:
        """The panel's own /littlefs/config.json, as its web UI would save it."""
        return {
            "wifi_hostname": self.hostname,
            "wifi_ssid": "qemu",  # anything non-empty; the QEMU build uses Ethernet
            "wifi_psk": "qemu",
            "mqtt_server": QEMU_HOST_ADDRESS,
            "mqtt_port": self.broker.panel_port,  # through FaultProxy, so tests can cut it
            "mqtt_username": "",
            "mqtt_password": "",
            "log_level": 4,  # ESP_LOG_DEBUG
            # The tests read the panel's log from its serial console. By default (Auto) the
            # panel stops writing it there once MQTT is connected.
            "serial_log": 1,  # ConfigManager::SerialLogMode::ALWAYS
        }

    @property
    def mac(self) -> str:
        return self.panel.mac

    def start(self) -> None:
        self.panel.start()

    def boot(self) -> None:
        """Start the panel and wait until it is registered, configured and idle on its screensaver."""
        self.start()
        self.wait_until_booted()

    def wait_until_booted(self) -> None:
        """The second half of boot(), for starting several panels before waiting on any."""
        self.manager.wait_for_registration(timeout=BOOT_TIMEOUT)
        if self.stack is not None:
            self.manager.accept()
        self.panel.wait_for_log(r"Manager is online!", timeout=60)
        if self.nextion.terminal_profile is not None:
            # The LVGL UI: a terminal session that has drawn something.
            self.panel.wait_for_log(r"NextionTerminal: Receiver ready", timeout=60)
            self.nextion.wait_for_rectangles(1, timeout=60)
        self.panel.wait_for_log(r"ScreensaverPage: Showing screensaver", timeout=60)
        # The manager's first config never matches the config.json written
        # above (button timings differ), so ButtonManager saves it to LittleFS
        # from the default loop. Under QEMU that takes ~20 s of guest time,
        # with the default loop stalled and flash cache off. Let it finish
        # before a test starts: MQTT traffic during it can overflow the
        # Ethernet RX ring (see "What differs from hardware" in the README),
        # and pages switched meanwhile unshow late.
        self.panel.wait_for_log(r"ConfigManager: Saved \d+ bytes to /littlefs/config.json", timeout=120)
        if self.nextion.terminal_profile is not None:
            # That save stops the guest long enough that the terminal session can time out
            # (the firmware's reverse timeout, on the guest's clock), and the firmware then
            # starts another. Let it, so that a test's first touch reaches a running session.
            time.sleep(TERMINAL_RESTART_GRACE)
            self.nextion.wait_for_terminal(timeout=60)
        self.panel.check_alive()

    def wake_to_home(self) -> None:
        """Touch the screensaver, as a user would, and wait for the home page to be drawn."""
        self.nextion.wake()
        self.nextion.wait_for_page("home")
        # The room label is the last thing HomePage::_update_display() writes
        # on first show; once it is there the page is fully rendered.
        self.nextion.wait_for(lambda n: n.get("home.room.txt") is not None, what="home page rendered")

    def set_tft(self, data: bytes) -> None:
        """Stage a TFT on the manager, and have the display accept it as a valid GUI."""
        self.update_server.data = data
        self.nextion.valid_md5s.add(self.update_server.md5)

    def status_report(self, since: int = 0, timeout: float = 60) -> "pb.NSPanelStatusReport":
        """The first status report the panel publishes after broker message index `since`."""
        msg = self.broker.wait_for_message("nspanel/%s/status_report" % self.mac, timeout=timeout, since=since)
        report = pb.NSPanelStatusReport()
        report.ParseFromString(msg.payload)
        return report

    def http_get(self, path: str, timeout: float = 30):
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (self.panel.web_port, path), timeout=timeout) as r:
            body = r.read()
            if r.headers.get_content_type() == "application/json":
                return r.status, json.loads(body)
            return r.status, body

    def stop(self) -> None:
        self.panel.stop()
        if self.stack is not None:
            try:
                if not os.environ.get("NSPM_STACK_KEEP_PANELS"):
                    self.manager.forget()
            finally:
                self.manager.close()
        if self.update_server is not None:
            self.update_server.close()
        self.broker.close()
