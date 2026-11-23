"""Simulated user input on the display, checked against what reaches the manager and the screen.

Touches are samples on the terminal display, which the firmware's LVGL widgets turn into actions.
"""

import json

import pytest

from nspm_qemu.proto import nspanel as pb
from shared_behaviour import (US_BRIGHTNESS_X0, US_BRIGHTNESS_X1, US_BRIGHTNESS_Y, US_ENTITIES_BUTTON, US_HOME_GAP, US_ROOM_FOOTER_BOX,
                              awake, lvgl_only, lvgl_us_only, pick_room, region, settle, tap_ceiling, wait_until_quiet)  # noqa: F401 (awake: a fixture)

Cmd = pb.NSPanelMQTTManagerCommand

# The tests tap where the widget is on the US screen (shared_behaviour.py).


# ---- The LVGL UI -------------------------------------------------------------------------------


@lvgl_only
def test_touching_screensaver_shows_home_page_on_lvgl(booted):
    mark = booted.panel.mark()
    booted.nextion.wake()

    booted.panel.wait_for_log(r"HomePage: Updating displayed values", since=mark, timeout=30)
    # The room it opens on is the default one: its state is what the page holds.
    booted.broker.wait_for_subscription(booted.panel.client_id, "nspanel/mqttmanager_10.0.2.2/room/1/state")
    booted.panel.wait_for_log(r"NextionTerminal: Backlight to 100%", since=mark, timeout=30)  # brightness restored
    booted.broker.wait_for_message("nspanel/%s/screen_state" % booted.mac, lambda m: m.payload == b"1")
    settle(booted.nextion)
    assert not booted.nextion.terminal.faults


@lvgl_us_only
@lvgl_only
def test_ceiling_tile_turns_on_ceiling_lights_on_lvgl(awake):
    turn_on = tap_ceiling(awake)
    assert turn_on.affect_lights == Cmd.CEILING_LIGHTS
    assert turn_on.selected_room == 1
    assert not getattr(turn_on, "global")  # `global` is a Python keyword
    # Lights were off and the slider cache was 0, so the panel asks for the configured default.
    assert turn_on.brightness_slider_value == awake.manager.config.default_light_brightess


@lvgl_us_only
@lvgl_only
def test_a_plain_tap_plays_only_the_touch_sound_on_lvgl(awake):
    # QEMU has no buzzer, but BuzzerManager logs each event it handles: one tap, one event, and not a
    # scene's (a tap on a scene raises SCENE_ACTIVATED instead of TOUCH, never both).
    settle(awake.nextion)
    mark = awake.panel.mark()
    tap_ceiling(awake)
    awake.panel.wait_for_log(r"BuzzerManager: Got TOUCH event", since=mark, timeout=30)
    settle(awake.nextion)
    assert len(awake.panel.log_lines(r"BuzzerManager: Got \w+ event", since=mark)) == 1


@lvgl_us_only
@lvgl_only
def test_brightness_slider_sends_slider_value_on_lvgl(awake):
    settle(awake.nextion)
    mark = awake.manager.command_mark()
    # A finger put down on the track sets the slider there; the panel acts on its release.
    x = round(US_BRIGHTNESS_X0 + 0.73 * (US_BRIGHTNESS_X1 - US_BRIGHTNESS_X0))
    awake.nextion.tap_screen(x, US_BRIGHTNESS_Y)

    cmd = awake.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark, timeout=30)
    assert abs(cmd.first_page_turn_on.brightness_slider_value - 73) <= 2  # a pixel is half a percent
    assert cmd.first_page_turn_on.affect_lights == Cmd.ALL


@lvgl_us_only
@lvgl_only
def test_room_list_shows_next_room_on_lvgl(awake):
    # The room list: opening it and picking the other room switches the home page's room.
    before = region(awake.nextion, US_ROOM_FOOTER_BOX)
    pick_room(awake, 1)

    awake.nextion.wait_for(lambda n: (region(n, US_ROOM_FOOTER_BOX) != before).any(), timeout=30,
                           what="the room's name redrawn")
    awake.broker.wait_for_subscription(awake.panel.client_id, "nspanel/mqttmanager_10.0.2.2/room/2/state")
    awake.panel.wait_for_log(r"RoomManager: Holding room topic nspanel/mqttmanager_10.0.2.2/room/2/state")
    assert tap_ceiling(awake).selected_room == 2


@lvgl_us_only
@lvgl_only
def test_room_state_from_manager_updates_display_on_lvgl(awake):
    settle(awake.nextion)
    before = region(awake.nextion, (8, 60, 156, 198))  # the ceiling tile
    room = awake.manager.rooms[1]
    room.num_ceiling_lights_on = 1
    room.ceiling_lights_dim_level = 40
    awake.manager.publish_room(1)

    awake.nextion.wait_for(lambda n: (region(n, (8, 60, 156, 198)) != before).any(), timeout=30,
                           what="the ceiling tile redrawn")


@lvgl_us_only
@lvgl_only
def test_room_entities_button_opens_entities_page_on_lvgl(awake):
    settle(awake.nextion)
    mark = awake.panel.mark()
    awake.nextion.tap_screen(*US_ENTITIES_BUTTON)

    awake.panel.wait_for_log(r"EntitiesPage: Received new data. Will update display", since=mark, timeout=30)
    awake.broker.wait_for_subscription(awake.panel.client_id, "nspanel/mqttmanager_10.0.2.2/entity_pages/11/state")
    settle(awake.nextion)
    awake.nextion.screenshot("entities")
    assert not awake.nextion.terminal.faults


@lvgl_us_only
@lvgl_only
def test_tap_burst_leaves_no_stuck_press_on_lvgl(awake):
    """Taps fired back to back (short holds, no pause) must not leave LVGL pressed: the next tap still clicks."""
    settle(awake.nextion)
    for _ in range(12):
        awake.broker.publish("nspanel/%s/command" % awake.mac,
                             json.dumps({"command": "lvgl_tap", "x": US_HOME_GAP[0], "y": US_HOME_GAP[1], "hold_ms": 20}))
    wait_until_quiet(awake)
    turn_on = tap_ceiling(awake)
    assert turn_on.affect_lights == Cmd.CEILING_LIGHTS
