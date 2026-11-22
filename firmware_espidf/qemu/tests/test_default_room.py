"""The home page following the manager's default room, and the tasks that touch it."""

import time

from nspm_qemu import gui
from nspm_qemu.fake_manager import Room
from shared_behaviour import (HOME, SCREENSAVER, awake, lvgl_only, lvgl_us_only, nextion_only, pick_room, region,
                              selected_room, settle, wait_until_quiet as _wait_until_quiet, US_ROOM_FOOTER_BOX,
                              wake_to_home)  # noqa: F401 (awake: a fixture)


def _set_default_room(rig, room_id):
    rig.manager.config.default_room = room_id
    rig.manager.publish_config()


@nextion_only
def test_changing_default_room_moves_home_page_showing_it(home):
    home.nextion.wait_for_value("home.room.txt", "Office")

    _set_default_room(home, 2)

    home.nextion.wait_for_value("home.room.txt", "Kitchen")


@nextion_only
def test_changing_default_room_leaves_other_rooms_alone(rig):
    """Only a panel showing the old default room follows the change."""
    rig.manager.rooms[3] = Room(3, "Lounge")
    rig.manager.config = rig.manager.default_config()
    rig.boot()
    rig.wake_to_home()
    rig.nextion.press(gui.HOME_NEXT_ROOM)  # the HMI reports its release only, see test_touch
    rig.nextion.wait_for_value("home.room.txt", "Kitchen")

    mark = rig.panel.mark()
    _set_default_room(rig, 3)
    rig.panel.wait_for_log(r"Received new config data from MQTT", since=mark, timeout=15)
    time.sleep(2)  # give the home page time to (wrongly) navigate

    assert rig.nextion.get("home.room.txt") == "Kitchen"


@nextion_only
def test_screen_on_during_config_pushes_does_not_crash(home):
    """Turning the screen on over MQTT while config updates arrive.

    Each screen_cmd asks for the home page from the MQTT task, while each
    config push runs HomePage::_handle_config_update() on the default loop.
    That pairing has deadlocked the MQTT task against the default loop (#50)
    and raced on HomePage's config pointer (#42). Interleave them hard and
    check the panel survives and stays responsive. Which room it ends
    on is not asserted: show() and the config handler can legitimately race
    over which default room was seen last.
    """
    screen_cmd = "nspanel/%s/screen_cmd" % home.mac
    for i in range(40):
        home.broker.publish(screen_cmd, "1")
        _set_default_room(home, 2 if i % 2 == 0 else 1)
        home.broker.publish(screen_cmd, "1")
        time.sleep(0.05)

    _wait_until_quiet(home)
    home.nextion.wait_for_page("home")

    mark = home.manager.command_mark()
    home.nextion.press(gui.HOME_CEILING)
    cmd = home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark, timeout=15)
    assert cmd.first_page_turn_on.selected_room in (1, 2)


@nextion_only
def test_screen_toggle_over_mqtt_then_default_room_change(home):
    """screen_cmd off and on, one at a time, then a default room change."""
    screen_cmd = "nspanel/%s/screen_cmd" % home.mac
    state = "nspanel/%s/screen_state" % home.mac
    for _ in range(3):
        mark = home.broker.mark()
        home.broker.publish(screen_cmd, "0")
        home.broker.wait_for_message(state, lambda m: m.payload == b"0", since=mark)
        home.nextion.wait_for(lambda n: n.page.startswith("screensaver"), what="screensaver shown")

        mark = home.broker.mark()
        home.broker.publish(screen_cmd, "1")
        home.broker.wait_for_message(state, lambda m: m.payload == b"1", since=mark)
        home.nextion.wait_for_page("home")
        home.nextion.wait_for_value("home.room.txt", "Office")

    # show() re-recorded the default room each time; a change must still be followed.
    _set_default_room(home, 2)
    home.nextion.wait_for_value("home.room.txt", "Kitchen")

    # And a screen-on afterwards keeps the panel on the new default room.
    mark = home.broker.mark()
    home.broker.publish(screen_cmd, "1")
    home.broker.wait_for_message(state, lambda m: m.payload == b"1", since=mark)
    time.sleep(1)
    assert home.nextion.get("home.room.txt") == "Kitchen"

    mark = home.manager.command_mark()
    home.nextion.press(gui.HOME_CEILING)
    cmd = home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark, timeout=15)
    assert cmd.first_page_turn_on.selected_room == 2


# ---- The LVGL UI -------------------------------------------------------------------------------
# The home page's room is a dropdown on the LVGL UI, where the Nextion's was a text field, so the
# room the page is on is read two ways: the firmware's own record of it (RoomManager logs the room
# topic it holds, and HomePage logs that it followed a default room change), and what a tap on the
# ceiling tile controls (selected_room of the command it sends the manager), which is the room the
# page really shows to its user.

DEFAULT_ROOM_CHANGED = r"HomePage: Default room page changed. Navigating to new default room"


def _holding(room_id):
    return r"RoomManager: Holding room topic nspanel/mqttmanager_10\.0\.2\.2/room/%d/state" % room_id


@lvgl_us_only
@lvgl_only
def test_changing_default_room_moves_home_page_showing_it_on_lvgl(awake):
    settle(awake.nextion)
    assert selected_room(awake) == 1
    before = region(awake.nextion, US_ROOM_FOOTER_BOX)
    mark = awake.panel.mark()

    _set_default_room(awake, 2)

    awake.panel.wait_for_log(DEFAULT_ROOM_CHANGED, since=mark, timeout=30)
    awake.panel.wait_for_log(_holding(2), since=mark, timeout=30)
    awake.nextion.wait_for(lambda n: (region(n, US_ROOM_FOOTER_BOX) != before).any(), timeout=30,
                           what="the room's name redrawn")
    assert selected_room(awake) == 2


@lvgl_us_only
@lvgl_only
def test_changing_default_room_leaves_other_rooms_alone_on_lvgl(rig):
    """Only a panel showing the old default room follows the change."""
    rig.manager.rooms[3] = Room(3, "Lounge")
    rig.manager.config = rig.manager.default_config()
    rig.manager.config.is_us_panel = True  # a new config forgets the rig's choice of layout (US: the one the touches are for)
    rig.boot()
    wake_to_home(rig)
    pick_room(rig, 1, count=3)  # Kitchen
    assert selected_room(rig) == 2

    mark = rig.panel.mark()
    _set_default_room(rig, 3)
    rig.panel.wait_for_log(r"Received new config data from MQTT", since=mark, timeout=15)
    time.sleep(2)  # give the home page time to (wrongly) navigate

    assert not rig.panel.log_lines(DEFAULT_ROOM_CHANGED, since=mark)
    assert not rig.panel.log_lines(_holding(3), since=mark)
    assert selected_room(rig) == 2


@lvgl_us_only
@lvgl_only
def test_screen_on_during_config_pushes_does_not_crash_on_lvgl(awake):
    """Turning the screen on over MQTT while config updates arrive (see the Nextion variant for why).
    Interleave them hard and check the panel survives and stays responsive."""
    screen_cmd = "nspanel/%s/screen_cmd" % awake.mac
    for i in range(40):
        awake.broker.publish(screen_cmd, "1")
        _set_default_room(awake, 2 if i % 2 == 0 else 1)
        awake.broker.publish(screen_cmd, "1")
        time.sleep(0.05)

    _wait_until_quiet(awake)
    settle(awake.nextion)

    # Responsive: a tap still reaches the manager, from whichever room the race left it on.
    assert selected_room(awake) in (1, 2)
    assert not awake.nextion.terminal.faults


@lvgl_us_only
@lvgl_only
def test_screen_toggle_over_mqtt_then_default_room_change_on_lvgl(awake):
    """screen_cmd off and on, one at a time, then a default room change."""
    screen_cmd = "nspanel/%s/screen_cmd" % awake.mac
    state = "nspanel/%s/screen_state" % awake.mac
    for _ in range(3):
        mark, log = awake.broker.mark(), awake.panel.mark()
        awake.broker.publish(screen_cmd, "0")
        awake.broker.wait_for_message(state, lambda m: m.payload == b"0", since=mark)
        awake.panel.wait_for_log(SCREENSAVER, since=log, timeout=30)

        mark, log = awake.broker.mark(), awake.panel.mark()
        awake.broker.publish(screen_cmd, "1")
        awake.broker.wait_for_message(state, lambda m: m.payload == b"1", since=mark)
        awake.panel.wait_for_log(HOME, since=log, timeout=30)
    settle(awake.nextion)
    assert selected_room(awake) == 1

    # show() re-recorded the default room each time; a change must still be followed.
    log = awake.panel.mark()
    _set_default_room(awake, 2)
    awake.panel.wait_for_log(DEFAULT_ROOM_CHANGED, since=log, timeout=30)
    awake.panel.wait_for_log(_holding(2), since=log, timeout=30)

    # And a screen-on afterwards keeps the panel on the new default room.
    mark = awake.broker.mark()
    awake.broker.publish(screen_cmd, "1")
    awake.broker.wait_for_message(state, lambda m: m.payload == b"1", since=mark)
    time.sleep(1)
    settle(awake.nextion)
    assert selected_room(awake) == 2
