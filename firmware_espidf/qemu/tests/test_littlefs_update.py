"""LittleFS updates under faults: whatever the download does, the panel keeps its settings.

The panel's config.json (Wi-Fi, MQTT, name) lives on the partition a LittleFS
update replaces. The update erases the partition before downloading, so a
download that fails afterwards used to leave the panel with factory defaults:
no Wi-Fi, no MQTT, fallback AP. On a real panel (Office, 2026-10-31) MQTT
dropped at 67% of the LittleFS download and the panel came back unconfigured.

These tests update LittleFS only: the panel's stored firmware md5 matches the
manager's, so "update firmware" goes straight to the LittleFS step. One test at
the end runs the whole firmware-then-LittleFS update.
"""

import os

import pytest

from nspm_qemu.ota_update import (check_fallback, check_new_image, check_settings_kept, make_littlefs, partition,
                                  run_update, stage_littlefs_only)
from nspm_qemu.rig import DEFAULT_BUILD_DIR

MARKER = "littlefs-update-test"
# A Django DEBUG=True error page, as the real manager serves on an exception.
ERROR_PAGE = b"<!DOCTYPE html><html><head><title>Server Error (500)</title></head><body>" + b"x" * 60000 + b"</body></html>"
# _update_littlefs_ota() tries this many times once the partition is erased (UpdateManager.hpp).
ATTEMPTS = 5


@pytest.fixture
def staged(rig):
    """A booted panel with a new LittleFS image staged on the manager, and its firmware already current."""
    stage_littlefs_only(rig, MARKER)
    rig.boot()
    return rig


def size(rig) -> int:
    return len(rig.tft_server.file("littlefs"))


def index_html(rig) -> bytes:
    status, body = rig.http_get("/")
    assert status == 200
    return body


# ---- success -----------------------------------------------------------------

def test_happy_path(staged):
    result = run_update(staged)
    check_settings_kept(staged, result)
    check_new_image(staged, result, MARKER)
    assert [d.outcome for d in result.downloads] == ["complete"], result.summary()


def test_reset_mid_download_is_retried(staged):
    """Office's failure: the connection drops two thirds of the way through, after the erase."""
    staged.tft_server.reset_at(int(size(staged) * 0.67), target="littlefs")
    result = run_update(staged)
    check_settings_kept(staged, result)
    check_new_image(staged, result, MARKER)
    assert [d.outcome for d in result.downloads] == ["reset", "complete"], result.summary()


def test_short_body_is_not_taken_as_complete(staged):
    """The manager closes cleanly before Content-Length, so the panel reads 0 bytes early.

    That used to end the download as if it were complete: a truncated image was
    kept as LittleFS, with its md5 stored.
    """
    staged.tft_server.fin_at(int(size(staged) * 0.5), target="littlefs")
    result = run_update(staged)
    check_settings_kept(staged, result)
    check_new_image(staged, result, MARKER)
    assert [d.outcome for d in result.downloads] == ["fin", "complete"], result.summary()


def test_partial_reads(staged):
    """Data arrives in small segments, as over a slow link."""
    staged.tft_server.trickle(512, 0.002)
    result = run_update(staged)
    check_settings_kept(staged, result)
    check_new_image(staged, result, MARKER)


# ---- every attempt fails -----------------------------------------------------

def test_download_keeps_failing_keeps_settings(staged):
    """Every attempt drops mid-download: the panel gives up, but writes its settings back first."""
    for _ in range(ATTEMPTS):
        staged.tft_server.reset_at(int(size(staged) * 0.5), target="littlefs")
    result = run_update(staged)
    check_settings_kept(staged, result)
    check_fallback(staged, result)
    assert [d.outcome for d in result.downloads] == ["reset"] * ATTEMPTS, result.summary()


def test_manager_down_after_erase_keeps_settings(staged):
    """The first attempt drops after the erase, and the manager then refuses every retry."""
    server = staged.tft_server
    server.reset_at(int(size(staged) * 0.3), target="littlefs")

    def during():
        server.wait_for_request(lambda r: r.target == "littlefs" and r.outcome == "reset", what="reset download")
        server.drop_next(ATTEMPTS, target="littlefs")

    result = run_update(staged, during=during)
    check_settings_kept(staged, result)
    check_fallback(staged, result)


# ---- refused before the erase ------------------------------------------------
# Nothing has been touched yet, so the panel keeps its current LittleFS, stays up,
# and does not reboot.

def check_untouched(rig, result, before: bytes) -> None:
    info = "\n" + result.summary()
    assert not result.rebooted, "panel rebooted after a refused LittleFS update" + info
    assert any("before the partition was erased" in line for line in result.console), info
    # The old filesystem is mounted again: the web UI still serves it, and saving settings still works.
    assert index_html(rig) == before, "the panel no longer serves its own index.html" + info
    assert MARKER.encode() not in before


def test_manager_unreachable_keeps_current_littlefs(staged):
    before = index_html(staged)
    staged.tft_server.drop_next(1, target="littlefs")
    check_untouched(staged, run_update(staged, expect_reboot=False), before)


def test_error_page_is_not_flashed(staged):
    """A 500 from Django must not be written to the partition as if it were the image."""
    before = index_html(staged)
    staged.tft_server.respond_next(1, 500, ERROR_PAGE, target="littlefs")
    check_untouched(staged, run_update(staged, expect_reboot=False), before)


def test_image_larger_than_partition_is_refused(staged):
    before = index_html(staged)
    staged.tft_server.set_file("littlefs", make_littlefs(staged, MARKER) + b"\xff" * 4096)
    check_untouched(staged, run_update(staged, expect_reboot=False), before)


def test_missing_content_length_is_refused(staged):
    """Without a size the panel cannot tell a whole image from a cut one, so it does not start."""
    before = index_html(staged)
    staged.tft_server.no_content_length_next(1, target="littlefs")
    check_untouched(staged, run_update(staged, expect_reboot=False), before)


# ---- the whole update: firmware, then LittleFS -------------------------------

@pytest.fixture
def staged_firmware(rig):
    """A booted panel with new firmware and a new LittleFS image staged on the manager."""
    with open(os.path.join(os.environ.get("NSPM_QEMU_BUILD_DIR", DEFAULT_BUILD_DIR), "firmware.bin"), "rb") as f:
        firmware = f.read()
    rig.tft_server.set_file("firmware", firmware)  # its md5 differs from the panel's stored one (none)
    rig.tft_server.set_file("littlefs", make_littlefs(rig, MARKER))
    rig.boot()
    return rig


def test_firmware_update_then_littlefs(staged_firmware):
    result = run_update(staged_firmware)
    check_settings_kept(staged_firmware, result)
    check_new_image(staged_firmware, result, MARKER)
    assert result.registration["md5_firmware"] == staged_firmware.tft_server.checksum("firmware"), result.summary()


def test_firmware_update_then_littlefs_keeps_failing(staged_firmware):
    """The firmware flashes fine, then every LittleFS attempt fails. update_firmware() used to reboot
    regardless, onto an erased LittleFS: the bug that wiped Office's settings."""
    for _ in range(ATTEMPTS):
        staged_firmware.tft_server.reset_at(int(size(staged_firmware) * 0.67), target="littlefs")
    result = run_update(staged_firmware)
    check_settings_kept(staged_firmware, result)
    check_fallback(staged_firmware, result)
    assert result.registration["md5_firmware"] == staged_firmware.tft_server.checksum("firmware"), result.summary()
