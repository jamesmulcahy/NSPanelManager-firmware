"""How the panel loads and saves its own /littlefs/config.json (lib/ConfigManager).

Only a missing file is replaced by the defaults at boot. A file that is there but cannot be used
(empty, bad JSON) is left exactly as it is while the panel serves its setup page, until the user
saves from that page. And a save that would write what the file already holds touches no flash,
so a config delivered again and again is written once.
"""

import json
import time
import urllib.parse
import urllib.request

import pytest

from nspm_qemu.ota_update import read_littlefs
from nspm_qemu.proto import nspanel as pb

SAVED = r"ConfigManager: Saved \d+ bytes to /littlefs/config\.json"
UNCHANGED = r"ConfigManager: config\.json unchanged, not written"


def wait_for_setup_page(rig, timeout: float = 120) -> None:
    """The panel's web UI answers, as it does once start_ap() has brought the link up."""
    deadline = time.monotonic() + timeout
    while True:
        rig.panel.check_alive()
        try:
            status, _ = rig.http_get("/", timeout=10)
            assert status == 200
            return
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(1)


def reboot_from_web_ui(rig) -> None:
    """GET /reboot and wait for the next boot. QEMU is started afresh on every reset, so the flash
    image read afterwards holds everything the panel wrote before it."""
    boots = rig.panel.boots
    try:
        rig.http_get("/reboot", timeout=10)
    except OSError:
        pass  # the panel may reset before the reply is read
    rig.panel.wait_for_reboot(boots)


UNREADABLE = [
    pytest.param(b"", id="empty"),
    pytest.param(b'{"wifi_ssid": "qemu", "wifi_psk": "qemu", "mqtt_ser', id="corrupt"),
]


@pytest.mark.parametrize("content", UNREADABLE)
def test_unreadable_config_is_left_alone_and_the_setup_page_is_served(rig, content):
    rig.panel.raw_config_json = content
    rig.start()

    rig.panel.wait_for_log(r"Main: config\.json could not be loaded", timeout=120)
    wait_for_setup_page(rig)
    assert not rig.panel.log_lines(r"Main: Default config values has been applied")
    assert not rig.panel.log_lines(SAVED)

    # A second boot finds the same file and does the same.
    mark = rig.panel.mark()
    reboot_from_web_ui(rig)
    rig.panel.wait_for_log(r"Main: config\.json could not be loaded", since=mark, timeout=120)
    wait_for_setup_page(rig)
    assert not rig.panel.log_lines(SAVED)

    files = read_littlefs(rig)
    assert files is not None, "LittleFS does not mount"
    assert files.get("config.json") == content, "config.json was changed: %r" % files.get("config.json")


def test_saving_from_the_setup_page_replaces_an_unreadable_config(rig):
    rig.panel.raw_config_json = b"{not json"
    rig.start()
    rig.panel.wait_for_log(r"Main: config\.json could not be loaded", timeout=120)
    wait_for_setup_page(rig)

    form = {
        "wifi_hostname": "qemu-panel",
        "log_level": "4",
        "serial_log": "1",
        "wifi_ssid": "saved-from-setup",
        "wifi_psk": "qemu",
        "mqtt_server": "",
        "mqtt_port": "1883",
        "mqtt_username": "",
        "mqtt_psk": "",
        "upload_protocol": "latest",
        "upload_baud_rate": "115200",
        "communication_baud_rate": "115200",
        "operating_baud_rate": "0",
    }
    boots = rig.panel.boots
    request = urllib.request.Request("http://127.0.0.1:%d/save_config" % rig.panel.web_port,
                                     data=urllib.parse.urlencode(form).encode(), method="POST")
    try:
        urllib.request.urlopen(request, timeout=30).read()
    except OSError:
        pass  # the panel restarts right after answering
    rig.panel.wait_for_reboot(boots)

    files = read_littlefs(rig)
    assert files is not None, "LittleFS does not mount"
    config = json.loads(files["config.json"])
    assert config["wifi_ssid"] == "saved-from-setup", config


def test_identical_config_deliveries_write_config_json_once(booted):
    """Thermostat mode stores the mode and temperatures on every config delivery. The first one
    that changes them is written; deliveries of the same config again are not."""
    rig = booted
    config = pb.NSPanelConfig()
    config.CopyFrom(rig.manager.config)
    config.button1_mode = pb.NSPanelConfig.THERMOSTAT_HEAT
    config.button1_lower_temperature = 19
    config.button1_upper_temperature = 22

    mark = rig.panel.mark()
    rig.manager.publish_config(config)
    rig.panel.wait_for_log(r"ButtonManager: Relay1 is set to thermostat \(heating\) mode", since=mark, timeout=60)
    rig.panel.wait_for_log(SAVED, since=mark, timeout=120)

    for _ in range(2):
        mark = rig.panel.mark()
        rig.manager.publish_config()
        rig.panel.wait_for_log(r"ButtonManager: Relay1 is set to thermostat \(heating\) mode", since=mark, timeout=60)
        rig.panel.wait_for_log(UNCHANGED, since=mark, timeout=60)
        assert not rig.panel.log_lines(SAVED, since=mark), "an identical config was written again"
    rig.panel.check_alive()
