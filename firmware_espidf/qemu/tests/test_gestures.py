"""Swipes on the LVGL UI (docs/LVGL.md, "Gestures"), through the firmware's own touch path (lvgl_tap).

- Swiping down from the top edge opens the menu from any page, not only Home; lower down it does nothing,
  so a drag there stays with what is under the finger.
- Swiping sideways on the screensaver turns its cards and leaves the panel asleep; a tap still wakes it.

Needs the LVGL UI (NSPM_QEMU_TERMINAL=us|eu-on-us; us is the default), and a schema with the screensaver
cards (see qemu/protobuf.ref).
"""

import json
import time

import pytest

from nspm_qemu.fake_nextion import TERMINAL
from nspm_qemu.proto import nspanel as pb
from test_lvgl_terminal import settle, swipe_down, wake_to_home
from test_screensaver_cards import configure, wait_for_turns

pytestmark = [
    pytest.mark.skipif(TERMINAL is None, reason="needs the LVGL UI: NSPM_QEMU_TERMINAL=us|eu-on-us (the default)"),
    pytest.mark.skipif(not hasattr(pb, "NSPanelCalendarUpdate"), reason="the schema has no screensaver cards (see protobuf.ref, NSPM_PROTO_DIR)"),
]

MENU = r"MenuPage: Showing the menu"
HOME = r"HomePage: Updating displayed values"


def swipe(rig, x, y, x2, y2, hold_ms=300):
    """A finger from x,y to x2,y2 over hold_ms (lvgl_tap clamps the end to the screen)."""
    rig.broker.publish("nspanel/%s/command" % rig.mac,
                       json.dumps({"command": "lvgl_tap", "x": x, "y": y, "x2": x2, "y2": y2, "hold_ms": hold_ms}))


def test_swipe_down_below_the_top_edge_does_not_open_the_menu(booted):
    rig = booted
    wake_to_home(rig)
    mark = rig.panel.mark()
    swipe(rig, 160, 150, 160, 440)
    time.sleep(5)
    assert not rig.panel.log_lines(MENU, since=mark)

    swipe_down(rig)  # from the top edge
    rig.panel.wait_for_log(MENU, since=mark, timeout=30)


def test_swipe_down_from_the_top_edge_opens_the_menu_from_the_entities_page(rig):
    rig.manager.config.default_page = pb.NSPanelConfig.ENTITIES
    rig.boot()
    events = rig.broker.event_mark()
    rig.nextion.wake()
    rig.broker.wait_for_subscription(rig.panel.client_id, rig.manager.base + "/entity_pages/11/state", since=events)
    settle(rig.nextion)

    # From the middle of the header bar (the page title), where the swipe starts on the page's own widgets.
    mark = rig.panel.mark()
    swipe(rig, 160, 20, 160, 300)
    rig.panel.wait_for_log(MENU, since=mark, timeout=30)
    settle(rig.nextion)

    # On the menu, the same swipe goes back to Home, as before.
    mark = rig.panel.mark()
    swipe_down(rig)
    rig.panel.wait_for_log(HOME, since=mark, timeout=30)
    assert not rig.nextion.terminal.faults


def test_swipes_turn_the_screensaver_cards_and_the_panel_sleeps_on(rig):
    # Long cards, so every turn seen here is a swipe's, not the rotation timer's.
    configure(rig, base_seconds=300, cards=((pb.NSPanelConfig.SCREENSAVER_CARD_CALENDAR, 300),))
    rig.boot()
    assert wait_for_turns(rig, 1, since=0)[0] == ("weather", 300)
    rig.nextion.wait_for_terminal(10)
    settle(rig.nextion)

    mark = rig.panel.mark()
    swipe(rig, 280, 200, 40, 200)  # left: the next card
    assert wait_for_turns(rig, 1, since=mark, timeout=30)[0] == ("calendar", 300)

    turned = rig.panel.mark()
    swipe(rig, 40, 200, 280, 200)  # right: the card before
    assert wait_for_turns(rig, 1, since=turned, timeout=30)[0] == ("weather", 300)

    time.sleep(3)
    assert not rig.panel.log_lines(HOME, since=mark), "a swipe woke the panel"

    # A tap still wakes it, on the release.
    wake_to_home(rig)
