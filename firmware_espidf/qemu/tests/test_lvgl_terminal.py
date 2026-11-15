"""The LVGL UI, on a display that runs nspanel-terminal's TFT.

Run with NSPM_QEMU_TERMINAL=us (or eu-on-us): the fake display is then nextion-tools' terminal
receiver, the firmware's LvglUi draws every page and streams the pixels, and touches arrive as
terminal touch samples (see fake_nextion.py). Without it these tests are skipped, and the rest of
the suite runs against NSPM's own TFT as usual.
"""

import io
import json
import struct
import time
import urllib.error
import urllib.request
import zlib

import pytest

from nspm_qemu.fake_nextion import TERMINAL

pytestmark = pytest.mark.skipif(TERMINAL is None, reason="needs NSPM_QEMU_TERMINAL=us|eu-on-us")


def settle(nextion, quiet: float = 1.5, timeout: float = 60) -> None:
    """Until a terminal session runs and its framebuffer has not changed for `quiet` seconds.

    A session can end and be restarted at any time under QEMU (a stalled guest misses the
    firmware's reverse timeout), and nothing changes while none runs: that is not settled.
    """
    deadline = time.time() + timeout
    last, since = None, time.time()
    while time.time() < deadline:
        time.sleep(0.1)
        terminal = nextion.terminal
        now = (id(terminal), terminal.changes) if nextion.terminal_active else None
        if now is None or now != last:
            last, since = now, time.time()
        elif time.time() - since >= quiet:
            return
    raise TimeoutError("the screen kept changing for %.0f s" % timeout)


def wake_to_home(rig) -> None:
    mark = rig.panel.mark()
    rig.nextion.wake()
    rig.panel.wait_for_log(r"HomePage: Updating displayed values", since=mark, timeout=30)
    settle(rig.nextion)


def test_boots_into_lvgl_ui(booted):
    nextion = booted.nextion
    booted.panel.wait_for_log(r"LvglUi: LVGL is the UI", timeout=5)
    assert nextion.showing_terminal
    assert not nextion.terminal.faults
    nextion.screenshot("boot")


def test_tap_wakes_to_home_page(booted):
    wake_to_home(booted)
    booted.nextion.screenshot("home")
    assert not booted.nextion.terminal.faults


# Where things are on the US home page, as the screen shows it (tests/.../screens/*-home.png).
US_CEILING_TILE = (82, 128)
US_CEILING_TILE_BOX = (8, 60, 156, 198)  # x0, y0, x1, y1
US_MODE_ALL = (281, 448)
US_FOOTER_BOX = (194, 424, 312, 472)


def region(nextion, box):
    x0, y0, x1, y1 = box
    return nextion.terminal_snapshot()[y0:y1, x0:x1].copy()


@pytest.fixture
def lvgl_home(booted):
    if TERMINAL != "us":
        pytest.skip("touch coordinates are for the US layout")
    wake_to_home(booted)
    return booted


def test_ceiling_tile_turns_on_ceiling_lights(lvgl_home):
    from nspm_qemu.proto import nspanel as pb
    Cmd = pb.NSPanelMQTTManagerCommand
    mark = lvgl_home.manager.command_mark()

    settle(lvgl_home.nextion)
    lvgl_home.nextion.tap_screen(*US_CEILING_TILE)

    cmd = lvgl_home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    assert cmd.first_page_turn_on.affect_lights == Cmd.CEILING_LIGHTS
    assert cmd.first_page_turn_on.selected_room == 1
    assert cmd.first_page_turn_on.brightness_slider_value == lvgl_home.manager.config.default_light_brightess
    assert not lvgl_home.nextion.terminal.faults


def test_room_state_from_manager_redraws_ceiling_tile(lvgl_home):
    settle(lvgl_home.nextion)
    before = region(lvgl_home.nextion, US_CEILING_TILE_BOX)
    room = lvgl_home.manager.rooms[1]
    room.num_ceiling_lights_on = 1
    room.ceiling_lights_dim_level = 40
    lvgl_home.manager.publish_room(1)

    lvgl_home.nextion.wait_for(lambda n: (region(n, US_CEILING_TILE_BOX) != before).any(), timeout=30,
                               what="the ceiling tile redrawn")
    settle(lvgl_home.nextion)
    lvgl_home.nextion.screenshot("ceiling-40")


def test_all_rooms_switch_redraws_footer(lvgl_home):
    settle(lvgl_home.nextion)
    before = region(lvgl_home.nextion, US_FOOTER_BOX)
    lvgl_home.nextion.tap_screen(*US_MODE_ALL)
    lvgl_home.nextion.wait_for(lambda n: (region(n, US_FOOTER_BOX) != before).any(), timeout=30,
                               what="the mode switch redrawn")
    settle(lvgl_home.nextion)
    lvgl_home.nextion.screenshot("all-rooms")
    lvgl_home.panel.wait_for_log(r"Received new home page state update for 'All rooms' mode", timeout=30)


# ---- GET /screenshot ---------------------------------------------------------------------------

SCREENSHOT_TIMEOUT = 180  # QEMU: whatever is still due crosses the ~8 KB/s link first


def get_screenshot(rig, fmt=None):
    """(status, headers, body) of GET /screenshot[?format=fmt]."""
    url = "http://127.0.0.1:%d/screenshot%s" % (rig.panel.web_port, "" if fmt is None else "?format=" + fmt)
    try:
        with urllib.request.urlopen(url, timeout=SCREENSHOT_TIMEOUT) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def upright_rgb565(nextion):
    """The receiver's framebuffer turned upright (as screenshots show it), as RGB565 values."""
    import numpy as np
    fb = nextion.framebuffer
    pixels = np.frombuffer(nextion.terminal_rgb565(), dtype=">u2").reshape(fb.height, fb.width)
    return np.ascontiguousarray(np.rot90(pixels, k=-(nextion.rotation // 90) % 4)).astype(np.uint16)


def widen(rgb565):
    """RGB565 values as 8-bit RGB, widened as the firmware's PNG and nextion-tools' snapshots do."""
    import numpy as np
    r, g, b = (rgb565 >> 11) & 31, (rgb565 >> 5) & 63, rgb565 & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.uint8)


def narrow(rgb):
    """8-bit RGB back to RGB565 values (exact for any widening that keeps the top bits)."""
    import numpy as np
    p = rgb.astype(np.uint16)
    return (p[..., 0] >> 3) << 11 | (p[..., 1] >> 2) << 5 | p[..., 2] >> 3


def png_chunks(data):
    """The PNG's chunks as (type, data), checking the signature and every CRC on the way."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    chunks, i = [], 8
    while i < len(data):
        (size,) = struct.unpack(">I", data[i:i + 4])
        kind, body = data[i + 4:i + 8], data[i + 8:i + 8 + size]
        (crc,) = struct.unpack(">I", data[i + 8 + size:i + 12 + size])
        assert crc == zlib.crc32(kind + body), "bad CRC on %r" % kind
        chunks.append((kind, body))
        i += 12 + size
    assert chunks[0][0] == b"IHDR" and chunks[-1] == (b"IEND", b"")
    return chunks


def capture_matching_display(rig, fmt, attempts=3):
    """A screenshot taken while the display held still, with the display's pixels from just
    before and after it (a minute ticking over on the clock can change the screen meanwhile)."""
    for _ in range(attempts):
        settle(rig.nextion)
        before = upright_rgb565(rig.nextion)
        status, headers, body = get_screenshot(rig, fmt)
        assert status == 200, body
        if (upright_rgb565(rig.nextion) == before).all():
            return headers, body, before
    pytest.fail("the display kept changing during the screenshot")


def test_screenshot_raw_matches_the_display(lvgl_home):
    import numpy as np
    headers, body, display = capture_matching_display(lvgl_home, "raw")
    assert headers["Content-Type"] == "application/octet-stream"
    assert headers["X-Pixel-Format"] == "RGB565LE"
    assert headers["Cache-Control"] == "no-store"
    w, h = int(headers["X-Width"]), int(headers["X-Height"])
    assert (w, h) == (320, 480) == (display.shape[1], display.shape[0])
    assert headers["X-Display-Streaming"] == "1"
    assert int(headers["X-Capture-Ms"]) > 0
    assert len(body) == w * h * 2
    shot = np.frombuffer(body, dtype="<u2").reshape(h, w)
    mismatched = int((shot != display).sum())
    assert mismatched == 0, "%d of %d pixels differ from the display" % (mismatched, w * h)


def test_screenshot_png_and_bmp_decode_to_the_display(lvgl_home):
    import numpy as np
    from PIL import Image

    headers, body, display = capture_matching_display(lvgl_home, None)  # PNG is the default
    assert headers["Content-Type"] == "image/png"
    assert headers["Content-Disposition"] == 'inline; filename="%s.png"' % lvgl_home.hostname
    chunks = png_chunks(body)
    assert [k for k, _ in chunks].count(b"IDAT") >= 1
    with Image.open(io.BytesIO(body)) as im:
        assert im.mode == "RGB" and im.size == (320, 480)
        png = np.asarray(im)
    assert (png == widen(display)).all()
    # What nextion-tools shows for the same screen, widened its own way.
    assert (png == lvgl_home.nextion.terminal_snapshot()).all()
    # Kept with the test's logs, as samples of each format.
    sample_dir = lvgl_home.panel.workdir
    with open("%s/screenshot.png" % sample_dir, "wb") as f:
        f.write(body)

    headers, body, display = capture_matching_display(lvgl_home, "bmp")
    assert headers["Content-Type"] == "image/bmp"
    assert len(body) == 66 + 320 * 480 * 2
    with Image.open(io.BytesIO(body)) as im:
        assert im.size == (320, 480)
        bmp = np.asarray(im.convert("RGB"))
    assert (narrow(bmp) == display).all()
    with open("%s/screenshot.bmp" % sample_dir, "wb") as f:
        f.write(body)


def test_screenshot_leaves_the_ui_responsive(lvgl_home):
    from nspm_qemu.proto import nspanel as pb
    status, _, _ = get_screenshot(lvgl_home, "png")
    assert status == 200
    # Right after it: a tap still lands, and the display still takes what changes.
    mark = lvgl_home.manager.command_mark()
    lvgl_home.nextion.tap_screen(*US_CEILING_TILE)
    cmd = lvgl_home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    assert cmd.first_page_turn_on.affect_lights == pb.NSPanelMQTTManagerCommand.CEILING_LIGHTS

    before = region(lvgl_home.nextion, US_FOOTER_BOX)
    lvgl_home.nextion.tap_screen(*US_MODE_ALL)
    lvgl_home.nextion.wait_for(lambda n: (region(n, US_FOOTER_BOX) != before).any(), timeout=30,
                               what="the mode switch redrawn after a screenshot")
    # And the next screenshot shows that change too.
    headers, body, display = capture_matching_display(lvgl_home, "raw")
    import numpy as np
    assert (np.frombuffer(body, dtype="<u2").reshape(480, 320) == display).all()
    assert not lvgl_home.nextion.terminal.faults
    lvgl_home.panel.wait_for_log(r"WebManager: Screenshot raw 320x480", timeout=10)


def test_screenshot_rejects_an_unknown_format(booted):
    status, headers, body = get_screenshot(booted, "jpeg")
    assert status == 400 and b"png, bmp or raw" in body


# ---- The look picker (docs/LVGL.md, "The look") ------------------------------------------------

# On the US screen (test/host/lvgl_sim: us-17-menu, us-19-look).
US_MENU_LOOK = (238, 267)
US_LOOKS = {"ocean": (82, 127), "dusk": (238, 127), "aurora": (82, 267), "peaks": (238, 267),
            "dark": (82, 407), "grey": (238, 407)}


def swipe_down(rig) -> None:
    """A swipe down the middle of the screen, through the firmware's own touch path (lvgl_tap)."""
    rig.broker.publish("nspanel/%s/command" % rig.mac,
                       json.dumps({"command": "lvgl_tap", "x": 160, "y": 60, "x2": 160, "y2": 300, "hold_ms": 300}))


def open_look_picker(rig) -> None:
    mark = rig.panel.mark()
    swipe_down(rig)
    rig.panel.wait_for_log(r"MenuPage: Showing the menu", since=mark, timeout=30)
    settle(rig.nextion)
    rig.nextion.screenshot("menu")
    mark = rig.panel.mark()
    rig.nextion.tap_screen(*US_MENU_LOOK)
    rig.panel.wait_for_log(r"LookPage: Showing the look picker", since=mark, timeout=30)
    settle(rig.nextion)
    rig.nextion.screenshot("look-picker")


def test_register_request_lists_the_lvgl_look_feature(booted):
    assert "lvgl_look" in booted.manager.register_requests[0].get("features", [])


def test_look_picked_on_the_panel_is_applied_reported_and_confirmed(lvgl_home):
    import numpy as np
    rig = lvgl_home
    open_look_picker(rig)
    before = upright_rgb565(rig.nextion)
    reports, mark = rig.manager.look_mark(), rig.panel.mark()

    rig.nextion.tap_screen(*US_LOOKS["dusk"])

    rig.panel.wait_for_log(r"LvglUi: Page background: dusk; theme: auto\.", since=mark, timeout=30)
    assert rig.manager.wait_for_look_report(since=reports) == {"background": "dusk", "theme": "auto"}
    # The fake manager answers as MQTTManager does: the config again, with the panel's look.
    rig.panel.wait_for_log(r"LvglLook: The manager confirmed look dusk/auto", since=mark, timeout=60)
    rig.nextion.wait_for(lambda n: (upright_rgb565(n) != before).mean() > 0.5, timeout=60,
                         what="the screen redrawn in the new look")
    settle(rig.nextion)
    rig.nextion.screenshot("look-dusk")
    # Still the picker, now in dusk, with nothing applied twice.
    assert len(rig.panel.log_lines(r"LvglUi: Page background:", since=mark)) == 1
    assert not rig.nextion.terminal.faults


def test_stale_config_does_not_undo_a_look_picked_on_the_panel(lvgl_home):
    rig = lvgl_home
    rig.manager.confirm_looks = False  # A manager that has not seen the report yet
    open_look_picker(rig)
    reports, mark = rig.manager.look_mark(), rig.panel.mark()
    rig.nextion.tap_screen(*US_LOOKS["peaks"])
    assert rig.manager.wait_for_look_report(since=reports) == {"background": "peaks", "theme": "auto"}
    rig.panel.wait_for_log(r"LvglUi: Page background: peaks; theme: auto\.", since=mark, timeout=30)

    # A config sent before the manager had the report (a settings save crossing the pick).
    rig.manager.lvgl_look = {"theme": "auto", "background": "ocean"}
    rig.manager.publish_config()
    rig.panel.wait_for_log(r"LvglLook: Config has look 'ocean'/'auto', not the panel's unconfirmed peaks/auto: keeping the panel's",
                           since=mark, timeout=60)
    assert not rig.panel.log_lines(r"LvglUi: Page background: ocean", since=mark)

    # The manager has the report now: its config carries the pick, which confirms it.
    rig.manager.lvgl_look = {"theme": "auto", "background": "peaks"}
    rig.manager.publish_config()
    rig.panel.wait_for_log(r"LvglLook: The manager confirmed look peaks/auto", since=mark, timeout=60)
    assert len(rig.panel.log_lines(r"LvglUi: Page background:", since=mark)) == 1

    # Confirmed: the manager's next change is applied again.
    mark = rig.panel.mark()
    rig.manager.lvgl_look = {"theme": "grey", "background": "none"}
    rig.manager.publish_config()
    rig.panel.wait_for_log(r"LvglUi: Page background: none; theme: grey\.", since=mark, timeout=60)


def test_look_from_the_config_is_applied_and_not_reported(booted):
    rig = booted
    reports, mark = rig.manager.look_mark(), rig.panel.mark()
    rig.manager.lvgl_look = {"theme": "auto", "background": "aurora"}
    rig.manager.publish_config()
    rig.panel.wait_for_log(r"LvglUi: Page background: aurora; theme: auto\.", since=mark, timeout=60)
    # The same config again changes nothing.
    rig.manager.publish_config()
    time.sleep(5)
    assert len(rig.panel.log_lines(r"LvglUi: Page background:", since=mark)) == 1
    assert rig.manager.look_mark() == reports
