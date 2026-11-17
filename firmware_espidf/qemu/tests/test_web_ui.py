"""The setup web UI is part of the firmware image: served from flash, reported as embedded, and
never written to LittleFS by an update.

The harness builds every test's LittleFS partition from a config.json alone, so each boot test
already proves the page comes from the app. These tests look at how it is sent, what the panel
reports about it, what a firmware update does (and does not do) to LittleFS, and a partition with
nothing on it at all.
"""

import gzip
import os
import time
import urllib.request

import pytest

from nspm_qemu.ota_update import check_firmware_updated, run_update
from nspm_qemu.rig import DEFAULT_BUILD_DIR, PROJECT_DIR

EMBEDDED = "embedded"
ASSETS = [
    ("/", "text/html", "index.html"),
    ("/static/main.css", "text/css", os.path.join("static", "main.css")),
]


def raw_get(rig, path: str, timeout: float = 30):
    """(status, headers, body), the body exactly as sent: urllib does not decompress."""
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (rig.panel.web_port, path), timeout=timeout) as r:
        return r.status, r.headers, r.read()


def source(relative_path: str) -> bytes:
    with open(os.path.join(PROJECT_DIR, "web", relative_path), "rb") as f:
        return f.read()


def check_asset(rig, path: str, content_type: str, relative_path: str) -> None:
    status, headers, body = raw_get(rig, path)
    assert status == 200
    assert headers.get_content_type() == content_type
    assert headers["Content-Encoding"] == "gzip"
    assert int(headers["Content-Length"]) == len(body)
    assert gzip.decompress(body) == source(relative_path), "%s does not decompress to web/%s" % (path, relative_path)


@pytest.mark.parametrize("path,content_type,relative_path", ASSETS)
def test_asset_is_served_gzipped_from_flash(booted, path, content_type, relative_path):
    check_asset(booted, path, content_type, relative_path)


def test_register_request_and_status_report_say_embedded(booted):
    assert booted.manager.register_requests[-1]["md5_data_file"] == EMBEDDED
    report = booted.status_report()
    assert report.md5_littlefs == EMBEDDED


# ---- a firmware update with a LittleFS image staged ---------------------------

@pytest.fixture
def staged_firmware(rig):
    """A booted panel with new firmware staged on the manager, and a LittleFS image beside it."""
    with open(os.path.join(os.environ.get("NSPM_QEMU_BUILD_DIR", DEFAULT_BUILD_DIR), "firmware.bin"), "rb") as f:
        rig.update_server.set_file("firmware", f.read())  # its md5 differs from the panel's stored one (none)
    # What an old manager would have the panel write over its settings.
    rig.update_server.set_file("littlefs", b"\xa5" * 4096)
    rig.boot()
    return rig


def test_firmware_update_leaves_littlefs_alone(staged_firmware):
    rig = staged_firmware
    result = run_update(rig)
    check_firmware_updated(rig, result)

    asked = [r.path for r in result.requests]
    assert any(p.startswith("/download_firmware") for p in asked), asked
    assert not [p for p in asked if "data_file" in p], "the panel asked for the LittleFS image: %s" % asked
    assert not [line for line in result.console if "LittleFS update" in line], result.summary()

    # config.json keeps everything the panel was configured with. Only the checksums the update
    # itself stores differ, and no LittleFS md5 was stored: the stored one is never reported.
    config = result.config
    for key, value in rig.panel.panel_config.items():
        assert config.get(key) == value, "config.json %s=%r, was %r\n%s" % (key, config.get(key), value, result.summary())
    assert config.get("md5_data_file", "") == "", result.summary()
    # And the setup page is still the one in the app.
    check_asset(rig, "/", "text/html", "index.html")


# ---- nothing on the LittleFS partition ---------------------------------------

def test_blank_littlefs_is_formatted_and_the_setup_page_is_served(rig):
    """A flash that wrote no filesystem: the panel formats it, applies defaults, starts its setup
    network and serves the page, which is in the app and not on the partition."""
    rig.panel.blank_littlefs = True
    rig.start()

    rig.panel.wait_for_log(r"LittleFS: Could not mount LittleFS, formatting the partition", timeout=120)
    rig.panel.wait_for_log(r"Main: Default config values has been applied", timeout=120)
    rig.panel.wait_for_log(r"ConfigManager: Saved \d+ bytes to /littlefs/config.json", timeout=120)

    deadline = time.monotonic() + 120
    while True:
        rig.panel.check_alive()
        try:
            check_asset(rig, "/", "text/html", "index.html")
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(1)
    check_asset(rig, "/static/main.css", "text/css", os.path.join("static", "main.css"))
