"""A display that misses the NSPM banner at boot.

NSPM's legacy TFT prints its "NSPM" banner once, when it starts. A panel that misses it (the display
came up late, bytes were lost while the UART settled) must not take NSPM's TFT for an unknown display
and sit on a blank screen: Nextion::init() power-cycles a display that sent no banner and is not a
terminal TFT, which makes NSPM's TFT print the banner again, and listens again (twice at most).
A terminal TFT, which prints no banner by design, is never power-cycled for it.

These start the display on the TFT they name (make_rig(display=...)), so they run in either
NSPM_QEMU_TERMINAL mode.
"""

import pytest

from nspm_qemu.proto import nspanel as pb

LEGACY_TEXT = "Display update needed"
RETRY = r"Nextion: No 'NSPM'-flag and the display is not a known terminal TFT"
MISSED = r"Nextion: Did not receive the 'NSPM'-flag"
BANNER = b"NSPM\xff\xff\xff"


def withhold_banner(rig, boots):
    """The display's first `boots` starts print no banner (the banner is missed); later ones do."""
    nextion = rig.nextion
    nextion.banner = BANNER
    original = nextion.boot
    started = []

    def boot():
        started.append(1)
        nextion.banner = b"" if len(started) <= boots else BANNER
        original()

    nextion.boot = boot
    return started


def test_a_banner_missed_on_the_first_start_is_heard_on_the_next(make_rig):
    rig = make_rig(display="hmi")
    withhold_banner(rig, 1)
    rig.boot()

    assert len(rig.panel.log_lines(MISSED)) == 1, rig.panel.log_lines(MISSED)
    assert len(rig.panel.log_lines(RETRY)) == 1, rig.panel.log_lines(RETRY)
    rig.panel.wait_for_log(r"Nextion: Received 'NSPM'-flag", timeout=5)
    rig.panel.wait_for_log(r"LvglUi: The display runs NSPM's legacy TFT", timeout=5)

    request = rig.manager.register_requests[0]
    assert request["features"] == ["lvgl_look", "no_hmi"], request
    report = rig.status_report(since=0)
    assert report.display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_NEXTION, report
    rig.nextion.wait_for_value("bootscreen.t_loading.txt", LEGACY_TEXT, timeout=60)
    assert rig.nextion.page == "bootscreen"


def test_a_banner_missed_twice_is_heard_on_the_last_try(make_rig):
    rig = make_rig(display="hmi")
    withhold_banner(rig, 2)
    rig.boot()

    assert len(rig.panel.log_lines(RETRY)) == 2, rig.panel.log_lines(RETRY)
    assert rig.manager.register_requests[0]["features"] == ["lvgl_look", "no_hmi"]
    assert rig.status_report(since=0).display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_NEXTION
    rig.nextion.wait_for_value("bootscreen.t_loading.txt", LEGACY_TEXT, timeout=60)


def test_a_display_that_never_sends_the_banner_gives_up_with_no_ui(make_rig):
    rig = make_rig(display="hmi")
    withhold_banner(rig, 100)
    rig.boot()

    # Three starts in all: the first and two retries. Then the boot goes on, as it did before the retries.
    assert len(rig.panel.log_lines(RETRY)) == 2, rig.panel.log_lines(RETRY)
    rig.panel.wait_for_log(r"LvglUi: No UI: the display runs no terminal TFT", timeout=5)
    assert rig.status_report(since=0).display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_TERMINAL_IDLE
    assert not rig.panel.log_lines(r"Nextion: Received 'NSPM'-flag")
    # Nothing is written to a display the firmware does not know.
    assert rig.nextion.get("bootscreen.t_loading.txt") is None, rig.nextion.get("bootscreen.t_loading.txt")


def test_a_terminal_tft_is_never_power_cycled_for_the_banner(make_rig):
    rig = make_rig(display="us")
    rig.boot()

    assert not rig.panel.log_lines(RETRY), rig.panel.log_lines(RETRY)
    assert len(rig.panel.log_lines(r"Nextion: Power-cycling Nextion display")) == 1
    assert rig.status_report(since=0).display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_LVGL
    assert rig.manager.register_requests[0]["features"] == ["lvgl_look", "no_hmi"]
