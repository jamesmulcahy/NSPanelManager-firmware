"""The LVGL UI, on a display that runs nspanel-terminal's TFT.

Run with NSPM_QEMU_TERMINAL=us (or eu-on-us): the fake display is then nextion-tools' terminal
receiver, the firmware's LvglUi draws every page and streams the pixels, and touches arrive as
terminal touch samples (see fake_nextion.py). Without it these tests are skipped, and the rest of
the suite runs against NSPM's own TFT as usual.
"""

import time

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
