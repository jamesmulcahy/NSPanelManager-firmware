"""The firmware against the real NSPanelManager, from the NSPanelManager repo's dev stack.

Start the stack (docker/dev/nspm-dev up in NSPanelManager), then point the harness at it:

    NSPM_STACK_MQTT=127.0.0.1:<broker port> qemu/run_tests.sh -m stack

Unlike the other tests, nothing here scripts the manager's side. Each panel joins as a
new panel, is accepted through the web UI's API, and every assertion is on what the real
MQTTManager sent, so a protocol change on either side shows up.

The panel runs the LVGL UI on the terminal TFT (NSPM_QEMU_TERMINAL=us, the default). A new panel is a
US one unless the stack's default panel type says otherwise, so the manager's config asks for the US
layout and taps land where test_lvgl_terminal.py's do.
"""

import uuid

import pytest

from nspm_qemu.fake_nextion import TERMINAL
from nspm_qemu.proto import nspanel as pb
from nspm_qemu.stack_manager import Stack
from nspm_qemu.stack_web import StackWeb
from test_lvgl_terminal import US_CEILING_TILE, settle, wake_to_home

pytestmark = [
    pytest.mark.stack,
    pytest.mark.skipif(TERMINAL != "us", reason="needs the LVGL UI on the US terminal TFT: NSPM_QEMU_TERMINAL=us (the default)"),
]

Cmd = pb.NSPanelMQTTManagerCommand


@pytest.fixture
def ceiling_light():
    """A ceiling light in the room new panels are accepted into, removed after the test.
    The panel's ceiling button does nothing in a room without ceiling lights, and the
    dev stack seeds none. List it before the panel fixture, so the panel boots with it."""
    stack = Stack.from_env()
    if stack is None:
        pytest.skip("no dev stack; set NSPM_STACK_MQTT=host:port")
    web = StackWeb(stack.web_url)
    room_id = min(r["id"] for r in web.rooms())
    light_id = web.add_light(room_id, "qemu-light-" + uuid.uuid4().hex[:8])
    yield light_id
    web.delete_entity(light_id)


@pytest.fixture
def home(booted):
    """A panel woken from its screensaver to the home page, and drawn."""
    wake_to_home(booted)
    return booted


# Where the room's name is drawn on the US home page, as the screen shows it: left of the room picker's arrow.
ROOM_PICKER_BOX = (20, 436, 100, 460)  # x0, y0, x1, y1


def press_ceiling_tile(home) -> None:
    settle(home.nextion)
    home.nextion.tap_screen(*US_CEILING_TILE)


def test_new_panel_is_accepted_and_configured(booted):
    accept = booted.manager.wait_for_accept()
    assert accept["config_topic"] == "nspanel/%s/config" % booted.mac

    config = booted.manager.config
    assert config.nspanel_id == booted.manager.panel_id
    # A new panel keeps the name it registered with.
    assert config.name == booted.hostname

    status = booted.manager.web.wait_for_panel_status(booted.mac, lambda s: s.get("state") == "online")
    assert status["accepted"]


def test_home_page_shows_the_managers_room(booted):
    room = booted.manager.room_status(booted.manager.config.default_room)
    assert room.name

    wake_to_home(booted)
    settle(booted.nextion)

    # The room's name is drawn in the footer's room picker, left of the mode switch: text on the card.
    x0, y0, x1, y1 = ROOM_PICKER_BOX
    pixels = booted.nextion.terminal_snapshot()[y0:y1, x0:x1]
    picker = pixels.reshape(-1, pixels.shape[2])
    background = max(set(map(tuple, picker)), key=lambda c: (picker == c).all(axis=1).sum())
    drawn = int((picker != background).any(axis=1).sum())
    assert drawn > 100, "no text in the room picker (%d pixels differ from its background)" % drawn
    booted.nextion.screenshot("room-picker")


def test_ceiling_button_reaches_the_manager(ceiling_light, home):
    mark = home.manager.command_mark()

    press_ceiling_tile(home)

    cmd = home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    assert cmd.first_page_turn_on.affect_lights == Cmd.CEILING_LIGHTS
    assert cmd.first_page_turn_on.selected_room == home.manager.config.default_room


def test_several_panels_join_together(make_rig):
    rigs = [make_rig() for _ in range(2)]
    for r in rigs:
        r.start()
    for r in rigs:
        r.wait_until_booted()

    ids = {r.manager.panel_id for r in rigs}
    assert len(ids) == len(rigs)
    assert {r.manager.config.nspanel_id for r in rigs} == ids


def test_survives_a_manager_restart(ceiling_light, home):
    """Restarting MQTTManager (as saving settings in the web UI does) takes the manager
    offline and back. The panel must notice both and keep working afterwards."""
    log_mark = home.panel.mark()

    home.manager.restart()

    home.panel.wait_for_log(r"Manager is offline!", since=log_mark, timeout=60)
    home.panel.wait_for_log(r"Manager is online!", since=log_mark, timeout=120)

    mark = home.manager.command_mark()
    press_ceiling_tile(home)
    home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
