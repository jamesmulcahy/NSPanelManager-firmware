"""The home page following the manager's default room, and the tasks that touch it."""

import time

from nspm_qemu import gui
from nspm_qemu.fake_manager import Room


def _wait_until_quiet(rig, quiet=2.0, timeout=60):
    """Wait until the panel has logged nothing for `quiet` seconds.

    After a burst the default event loop works through a backlog, and a touch
    posted while its queue is full is dropped (Nextion posts it with a 16 ms
    timeout). A person would tap again; a test should wait instead.
    """
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


def _set_default_room(rig, room_id):
    rig.manager.config.default_room = room_id
    rig.manager.publish_config()


def test_changing_default_room_moves_home_page_showing_it(home):
    home.nextion.wait_for_value("home.room.txt", "Office")

    _set_default_room(home, 2)

    home.nextion.wait_for_value("home.room.txt", "Kitchen")


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
