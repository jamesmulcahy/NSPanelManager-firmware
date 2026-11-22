"""What the shared-behaviour tests (boot, touch, default room, reconnects, page switches, weather)
need to run on both displays: NSPM's own Nextion TFT, and the LVGL UI on a terminal display.

Each of those tests keeps its Nextion variant (marked `nextion_only`) and has an LVGL one beside it
(marked `lvgl_only`), asserting the same behaviour through what the LVGL UI shows: log lines, the
screen's pixels, and the commands a tap sends the manager. Not a test module itself: the tests
import from it.

Where things are on the US home page, as the screen shows it (tests/.../screens/*-home.png):

    header      scenes (top left), the room's entities "..." (top right, 288,30)
    tiles       ceiling (82,128), table (238,128)
    sliders     brightness (x 54..254, y 258), colour temperature (y 366)
    footer      the room list (90,448; it opens upwards), the Room/All switch
"""

import json
import time

import pytest

from nspm_qemu.fake_nextion import TERMINAL
from test_lvgl_terminal import region, settle, wake_to_home  # noqa: F401 (re-exported)

nextion_only = pytest.mark.skipif(TERMINAL is not None, reason="NSPM's TFT only (NSPM_QEMU_TERMINAL=nextion)")
lvgl_only = pytest.mark.skipif(TERMINAL is None, reason="needs the LVGL UI: NSPM_QEMU_TERMINAL=us|eu-on-us (the default)")
# Touch coordinates below are for the US layout.
lvgl_us_only = pytest.mark.skipif(TERMINAL not in (None, "us"), reason="touch coordinates are for the US layout")

HOME = r"HomePage: Updating displayed values"
SCREENSAVER = r"ScreensaverPage: Showing screensaver page"

US_CEILING_TILE = (82, 128)
US_BRIGHTNESS_X0, US_BRIGHTNESS_X1, US_BRIGHTNESS_Y = 54, 254, 258
US_ENTITIES_BUTTON = (288, 30)
US_ROOM_LIST = (90, 448)
US_ROOM_FOOTER_BOX = (8, 424, 186, 472)  # x0, y0, x1, y1: the room list's button
US_ROW_HEIGHT = 42  # of the room list that opened upwards; its last row is at y 401
US_LIST_LAST_ROW_Y = 401
# An empty strip of the home page, between the tiles and the sliders: a tap there wakes a
# screensaver and then lands on nothing.
US_HOME_GAP = (160, 203)


def wait_for_room_names(rig, count=2):
    """The room list is filled once RoomManager knows every room's name (it logs when it does)."""
    rig.panel.wait_for_log(r"RoomManager: Room names: %d of %d known" % (count, count), timeout=60)


def pick_room(rig, index, count=2):
    """Opens the home page's room list and taps the room at `index` of `count` rooms."""
    wait_for_room_names(rig, count)
    settle(rig.nextion)
    rig.nextion.tap_screen(*US_ROOM_LIST)
    settle(rig.nextion)
    rig.nextion.tap_screen(60, US_LIST_LAST_ROW_Y - (count - 1 - index) * US_ROW_HEIGHT)
    settle(rig.nextion)


def tap_ceiling(rig):
    """Taps the ceiling tile and returns the first_page_turn_on command it sends the manager."""
    settle(rig.nextion)
    mark = rig.manager.command_mark()
    rig.nextion.tap_screen(*US_CEILING_TILE)
    return rig.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on",
                                        since=mark, timeout=30).first_page_turn_on


def selected_room(rig):
    """The room the home page is on, as the manager is told it: the room a ceiling tap controls."""
    return tap_ceiling(rig).selected_room


def swipe(rig, x, y, x2, y2, hold_ms=300):
    """A finger from x,y to x2,y2 over hold_ms, through the firmware's own touch path (lvgl_tap)."""
    rig.broker.publish("nspanel/%s/command" % rig.mac,
                       json.dumps({"command": "lvgl_tap", "x": x, "y": y, "x2": x2, "y2": y2, "hold_ms": hold_ms}))


def wait_until_quiet(rig, quiet=2.0, timeout=60):
    """Waits until the panel has logged nothing for `quiet` seconds.

    After a burst the default event loop works through a backlog, and a touch posted while its
    queue is full is dropped. A person would tap again; a test should wait instead."""
    deadline = time.monotonic() + timeout
    last = rig.panel.mark()
    since = time.monotonic()
    while time.monotonic() < deadline:
        time.sleep(0.25)
        rig.panel.check_alive()
        now = rig.panel.mark()
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= quiet:
            return
    raise TimeoutError("panel still busy after %.0fs" % timeout)


@pytest.fixture
def awake(booted):
    """A booted panel woken from its screensaver to the home page, on either display."""
    if TERMINAL is None:
        booted.wake_to_home()
    else:
        wake_to_home(booted)
    return booted
