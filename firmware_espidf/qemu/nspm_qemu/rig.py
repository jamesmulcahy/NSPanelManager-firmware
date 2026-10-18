"""One emulated panel with its broker, manager and display, ready for a test to drive."""

import json
import os
import urllib.request
from typing import Optional

from .broker import Broker
from .fake_manager import QEMU_HOST_ADDRESS, FakeManager
from .panel import QemuPanel

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_BUILD_DIR = os.path.join(PROJECT_DIR, ".pio", "build", "qemu")

# Generous, because CI runners are slow and QEMU runs the guest slower than
# real time. Boot to "registered" takes ~25 s of guest time; most of it is
# esp-mqtt's 10 s reconnect delay after its first attempt beats DHCP.
BOOT_TIMEOUT = 180


class Rig:
    def __init__(self, workdir: str, mac: str = "C0:49:EF:00:00:01", hostname: str = "qemu-panel",
                 build_dir: Optional[str] = None, panel_config: Optional[dict] = None):
        os.makedirs(workdir, exist_ok=True)
        self.broker = Broker(log_path=os.path.join(workdir, "mosquitto.log"))
        self.manager = FakeManager(self.broker)
        self.hostname = hostname
        if panel_config is None:
            panel_config = self.default_panel_config()
        self.panel = QemuPanel(workdir, build_dir or os.environ.get("NSPM_QEMU_BUILD_DIR", DEFAULT_BUILD_DIR),
                               PROJECT_DIR, mac, panel_config)
        self.nextion = self.panel.nextion
        # Any wait on a fake ends as soon as the firmware panics or QEMU exits.
        for fake in (self.broker, self.manager, self.nextion):
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
        }

    @property
    def mac(self) -> str:
        return self.panel.mac

    def start(self) -> None:
        self.panel.start()

    def boot(self) -> None:
        """Start the panel and wait until it is registered, configured and idle on its screensaver."""
        self.start()
        self.manager.wait_for_registration(timeout=BOOT_TIMEOUT)
        self.panel.wait_for_log(r"Manager is online!", timeout=60)
        self.panel.wait_for_log(r"ScreensaverPage: Showing screensaver", timeout=60)
        # The manager's first config never matches the config.json written
        # above (button timings differ), so ButtonManager saves it to LittleFS
        # from the default loop. Under QEMU that takes ~20 s of guest time,
        # with the default loop stalled and flash cache off. Let it finish
        # before a test starts: MQTT traffic during it can overflow the
        # Ethernet RX ring (see "What differs from hardware" in the README),
        # and pages switched meanwhile unshow late.
        self.panel.wait_for_log(r"ConfigManager: Saved \d+ bytes to /littlefs/config.json", timeout=120)
        self.panel.check_alive()

    def wake_to_home(self) -> None:
        """Touch the screensaver, as a user would, and wait for the home page to be drawn."""
        self.nextion.wake()
        self.nextion.wait_for_page("home")
        # The room label is the last thing HomePage::_update_display() writes
        # on first show; once it is there the page is fully rendered.
        self.nextion.wait_for(lambda n: n.get("home.room.txt") is not None, what="home page rendered")

    def http_get(self, path: str, timeout: float = 30):
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (self.panel.web_port, path), timeout=timeout) as r:
            body = r.read()
            if r.headers.get_content_type() == "application/json":
                return r.status, json.loads(body)
            return r.status, body

    def stop(self) -> None:
        self.panel.stop()
        self.broker.close()
