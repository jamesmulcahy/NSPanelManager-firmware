"""A panel whose display still runs NSPM's legacy (HMI) TFT.

The firmware has no HMI pages any more. On NSPM's TFT (the display sends the NSPM banner) it shows
no UI: only the TFT's own boot screen, with "Display update needed" and what the panel knows on
the line below, and it says so to the manager (register_request features "no_hmi",
display_mode DISPLAY_MODE_NEXTION). It stays a working panel meanwhile: MQTT, relays, the
temperature sensor, firmware updates and the GUI update, which installs the terminal TFT and brings
up the LVGL UI.

The display starts on NSPM's TFT whatever NSPM_QEMU_TERMINAL says (make_rig(display="hmi")), but
these run in CI's Nextion leg, like the other tests of NSPM's TFT (NSPM_QEMU_TERMINAL=nextion).
"""

import json
import os
import re
import urllib.error
import urllib.request

import pytest

from nspm_qemu import hmi
from nspm_qemu.fake_nextion import SLEEP, TERMINAL, WAKE
from nspm_qemu.gui_update import check_invariants, run_gui_update
from nspm_qemu.ota_update import check_firmware_updated, run_update
from nspm_qemu.proto import nspanel as pb
from nspm_qemu.rig import DEFAULT_BUILD_DIR
from test_gui_update import make_tft

pytestmark = pytest.mark.skipif(TERMINAL is not None, reason="NSPM's TFT only (NSPM_QEMU_TERMINAL=nextion)")

LEGACY_TEXT = "Display update needed"
HINT = "Update GUI in NSPanel Manager"
DOT = " · "
QEMU_IP = "10.0.2.15"  # QEMU user-mode DHCP lease
MAIN, SECOND = "bootscreen.t_loading.txt", "bootscreen.t_ip.txt"
# What the firmware writes to the legacy TFT, and all it may write: the boot screen's two fields, the
# page itself, the backlight, and the connect and baud handshake.
ALLOWED_COMMANDS = [
    r"^page bootscreen$",
    r"^t_(loading|ip)\.txt=\".*\"$",
    r"^dim=\d+$",
    r"^sleep=0$",
    r"^connect$",
    r"^bkcmd=\d$",
    r"^bauds?=\d+$",
    r"^printh a5 [0-9a-f]{2} ff ff ff$",  # NextionWindow's pacing marker, which the display echoes
    r"DRAKJHSUYDGBNCJHGJKSHBDN$",
    r"^$",
]


@pytest.fixture
def legacy(make_rig):
    """A panel whose display runs NSPM's TFT, not started yet."""
    return make_rig(display="hmi")


def texts_written(rig, component):
    """Every text the firmware assigned to the boot screen's `component`, in order."""
    rx = re.compile(r'^%s\.txt="(.*)"$' % component)
    return [m.group(1) for m in (rx.match(c) for c in list(rig.nextion.commands)) if m]


def assert_fits(text):
    """`text` fits the boot screen's text fields as the TFT draws them: within the field's width in its
    font, and within the text the field holds (txt_maxl, in bytes)."""
    page = hmi.load().page("bootscreen")
    for name in ("t_loading", "t_ip"):
        attrs = page.component(name).attrs
        font = hmi.load().fonts()[attrs["font"]]
        assert not font.missing(text), "the font of %s cannot draw %r of %r" % (name, font.missing(text), text)
        assert font.width(text) <= attrs["w"], "%r is %d px wide, %s only %d" % (text, font.width(text), name, attrs["w"])
        assert len(text.encode()) <= attrs["txt_maxl"], "%r is %d bytes, %s holds %d" % (
            text, len(text.encode()), name, attrs["txt_maxl"])


def assert_only_the_legacy_screen(rig):
    """The display was sent nothing but the boot screen: no HMI page or component the firmware no longer drives."""
    others = sorted({c for c in rig.nextion.commands if not any(re.search(p, c) for p in ALLOWED_COMMANDS)})
    assert not others, "the firmware sent the legacy TFT more than its boot screen: %r" % others[:20]
    assert rig.nextion.page == "bootscreen", rig.nextion.page
    assert not rig.panel.log_lines(r"BUG: "), rig.panel.log_lines(r"BUG: ")


def wait_for_second_line(rig, expected):
    rig.nextion.wait_for_value(SECOND, expected, timeout=60)


def test_boots_to_the_legacy_screen_and_says_it_has_no_hmi(legacy):
    rig = legacy
    rig.boot()

    request = rig.manager.register_requests[0]
    assert request["features"] == ["lvgl_look", "no_hmi"], request
    report = rig.status_report(since=0)
    assert report.display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_NEXTION, report
    rig.panel.wait_for_log(r"LvglUi: The display runs NSPM's legacy TFT", timeout=5)

    # The config names the panel: its name and address. The hint does not fit beside them.
    name = rig.manager.config.name
    wait_for_second_line(rig, name + DOT + QEMU_IP)
    assert rig.nextion.get(MAIN) == LEGACY_TEXT
    rig.nextion.screenshot("legacy-screen")

    # Best first, as the panel learnt it: blank without a network, then the address alone (the hint
    # does not fit beside it either), then the name too. Nothing else was ever on either line.
    assert set(texts_written(rig, "t_loading")) == {LEGACY_TEXT}, texts_written(rig, "t_loading")
    seconds = texts_written(rig, "t_ip")
    distinct = [t for i, t in enumerate(seconds) if i == 0 or seconds[i - 1] != t]
    assert distinct[-1] == name + DOT + QEMU_IP, distinct
    assert QEMU_IP in distinct and distinct.index(QEMU_IP) < distinct.index(name + DOT + QEMU_IP), distinct
    assert set(distinct) <= {"", QEMU_IP, name + DOT + QEMU_IP}, distinct
    for text in [LEGACY_TEXT] + distinct:
        assert_fits(text)
    # What the hint would have needed: it is dropped because it does not fit, not by accident.
    with pytest.raises(AssertionError):
        assert_fits(QEMU_IP + DOT + HINT)

    assert_only_the_legacy_screen(rig)


def test_a_name_too_long_to_fit_leaves_the_address(legacy):
    rig = legacy
    rig.manager.config.name = "Master Bedroom Window Panel"  # with the address: 40 bytes
    rig.boot()
    rig.nextion.wait_for_value(MAIN, LEGACY_TEXT, timeout=60)
    # The name is known (the panel took it as its hostname), and still only the address shows.
    rig.panel.wait_for_log(r"Updating internal WiFi hostname to match manager provided friendly name", timeout=60)
    wait_for_second_line(rig, QEMU_IP)
    assert not [t for t in texts_written(rig, "t_ip") if "Master" in t], texts_written(rig, "t_ip")
    assert_only_the_legacy_screen(rig)


def test_screenshot_is_409(legacy):
    # NSPM's TFT draws its boot screen itself: there is nothing on the ESP32 to capture.
    legacy.boot()
    with pytest.raises(urllib.error.HTTPError) as e:
        urllib.request.urlopen("http://127.0.0.1:%d/screenshot" % legacy.panel.web_port, timeout=30)
    assert e.value.code == 409
    assert b"LVGL" in e.value.read()


def test_the_displays_own_events_reach_no_page(legacy):
    """The HMI TFT's sleep, wake, touch and string frames, and the commands that drove its pages, find
    nothing to show: the boot screen stays, and nothing else is sent to the display."""
    rig = legacy
    rig.boot()
    rig.nextion.wait_for_value(SECOND, rig.manager.config.name + DOT + QEMU_IP, timeout=60)
    mark = rig.panel.mark()

    rig.nextion.send_frame(SLEEP)
    rig.panel.wait_for_log(r"InterfaceManager: No UI; ignoring the display's sleep event", since=mark, timeout=30)
    rig.nextion.send_frame(WAKE)
    rig.panel.wait_for_log(r"InterfaceManager: No UI; ignoring the display's wake event", since=mark, timeout=30)
    rig.nextion.send_touch_event(3, True, page="bootscreen")
    rig.nextion.send_touch_event(3, False, page="bootscreen")
    rig.nextion.send_string("hello")
    rig.broker.publish("nspanel/%s/screen_raw_command" % rig.mac, "page home")
    rig.panel.wait_for_log(r"screen_raw_command is no longer supported", since=mark, timeout=30)
    # The screen on and off over MQTT: off has no screensaver to go to, on brings the boot screen up again.
    rig.broker.publish("nspanel/%s/screen_cmd" % rig.mac, "0")
    rig.panel.wait_for_log(r"InterfaceManager: No UI; dropping page request", since=mark, timeout=30)
    commands = rig.nextion.mark()
    rig.broker.publish("nspanel/%s/screen_cmd" % rig.mac, "1")
    rig.nextion.wait_for_command(r"^page bootscreen$", since=commands, timeout=30)
    rig.nextion.wait_for_value(SECOND, rig.manager.config.name + DOT + QEMU_IP, timeout=30)
    assert rig.nextion.get(MAIN) == LEGACY_TEXT

    rig.panel.check_alive()
    assert_only_the_legacy_screen(rig)


def test_relays_brightness_and_temperature_work_on_the_legacy_screen(legacy):
    rig = legacy
    rig.boot()
    # Toggled ("2") rather than set: what the relay starts in depends on the config's default mode.
    states = []
    for _ in range(2):
        mark = rig.broker.mark()
        rig.broker.publish("nspanel/%s/relay1_cmd" % rig.mac, "2")
        states.append(rig.broker.wait_for_message("nspanel/%s/relay1_state" % rig.mac, since=mark, timeout=30).payload)
    assert sorted(states) == [b"0", b"1"], states

    rig.broker.publish("nspanel/%s/brightness_cmd" % rig.mac, "40")
    rig.nextion.wait_for(lambda n: n.system.get("dim") == 40, what="display dimmed to 40", timeout=30)

    # The sensor's first average, which waits for the config's unit, as on any panel.
    rig.panel.wait_for_log(r"StatusUpdateManager: Temperature: first average", timeout=120)
    report = rig.status_report(since=rig.broker.mark(), timeout=120)
    assert report.display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_NEXTION
    assert_only_the_legacy_screen(rig)


def test_lvgl_ui_command_is_ignored(legacy):
    rig = legacy
    rig.boot()
    ignored = r"UpdateManager: The lvgl_ui command is no longer supported; ignored"
    mark, boots = rig.panel.mark(), rig.panel.boots
    for enable in (True, False):
        rig.broker.publish("nspanel/%s/command" % rig.mac, json.dumps({"command": "lvgl_ui", "enable": enable}))
    rig.panel.wait_until(lambda: len(rig.panel.log_lines(ignored, since=mark)) == 2, timeout=30, what="both commands ignored")
    rig.panel.check_alive()
    assert rig.panel.boots == boots, "the lvgl_ui command rebooted the panel"
    assert_only_the_legacy_screen(rig)


def test_saved_lvgl_ui_false_is_ignored(legacy):
    """Older firmware kept the HMI pages with lvgl_ui false in config.json. There are none now: the
    setting is ignored, and the TFT decides (here: the legacy screen, as without it)."""
    rig = legacy
    rig.panel.panel_config["lvgl_ui"] = False
    rig.boot()
    rig.panel.wait_for_log(r"ConfigManager: lvgl_ui is false in config.json: no longer supported, ignored", timeout=5)
    rig.nextion.wait_for_value(MAIN, LEGACY_TEXT, timeout=60)
    assert rig.status_report(since=0).display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_NEXTION
    assert_only_the_legacy_screen(rig)


def test_firmware_update_works_on_the_legacy_screen(legacy):
    rig = legacy
    with open(os.path.join(os.environ.get("NSPM_QEMU_BUILD_DIR", DEFAULT_BUILD_DIR), "firmware.bin"), "rb") as f:
        rig.update_server.set_file("firmware", f.read())
    rig.boot()
    result = run_update(rig)
    check_firmware_updated(rig, result)
    # Back on the legacy screen, and still saying why.
    assert result.registration["features"] == ["lvgl_look", "no_hmi"]
    rig.nextion.wait_for_value(MAIN, LEGACY_TEXT, timeout=120)


def test_gui_update_installs_the_terminal_tft_and_lvgl_draws(legacy):
    """The way off the legacy screen: the manager's GUI update installs the terminal TFT, and after
    the reboot the LVGL UI draws on it."""
    rig = legacy
    rig.boot()
    assert_only_the_legacy_screen(rig)
    rig.set_tft(make_tft(61), installs="terminal")

    result = run_gui_update(rig)

    check_invariants(result)
    assert result.display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_LVGL, result.summary()
    assert rig.nextion.terminal_profile is not None, "the display still runs NSPM's TFT"
    rig.panel.wait_for_log(r"LvglUi: LVGL is the UI", timeout=120)
    rig.nextion.wait_for_terminal(timeout=120)
    rig.nextion.wait_for_rectangles(1, timeout=120)
    assert not rig.nextion.terminal.faults
    rig.panel.wait_for_log(r"ScreensaverPage: Showing screensaver", timeout=120)
    rig.nextion.screenshot("lvgl-after-update")
    assert not rig.panel.log_lines(r"BUG: ")
