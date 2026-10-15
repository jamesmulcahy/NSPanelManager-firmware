"""Simulated user input on the display, checked against what reaches the manager and the screen."""

from nspm_qemu.proto import nspanel as pb

# Component ids from lib/Pages/GUI_data.hpp (GUI_HOME_PAGE).
BUTTON_CEILING = 22
BUTTON_TABLE = 21
BRIGHTNESS_SLIDER = 2
BUTTON_ROOM_ENTITIES = 5
BUTTON_NEXT_ROOM = 10

Cmd = pb.NSPanelMQTTManagerCommand


def test_touching_screensaver_shows_home_page(booted):
    booted.nextion.wake()

    booted.nextion.wait_for_page("home")
    booted.nextion.wait_for_value("home.room.txt", "Office")
    booted.nextion.wait_for(lambda n: n.system.get("dim") == 100, what="display brightness restored")
    booted.broker.wait_for_message("nspanel/%s/screen_state" % booted.mac, lambda m: m.payload == b"1")


def test_ceiling_button_turns_on_ceiling_lights(home):
    mark = home.manager.command_mark()

    home.nextion.press(BUTTON_CEILING)

    cmd = home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    turn_on = cmd.first_page_turn_on
    assert turn_on.affect_lights == Cmd.CEILING_LIGHTS
    assert turn_on.selected_room == 1
    assert not getattr(turn_on, "global")  # `global` is a Python keyword
    # Lights were off and the slider cache was 0, so the panel asks for the configured default.
    assert turn_on.brightness_slider_value == home.manager.config.default_light_brightess
    assert cmd.nspanel_id == home.manager.nspanel_id


def test_brightness_slider_sends_slider_value(home):
    mark = home.manager.command_mark()

    # The panel reads the slider back with `get home.s_brightness.val` on release.
    home.nextion.slide("home.s_brightness", BRIGHTNESS_SLIDER, 73)

    cmd = home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    assert cmd.first_page_turn_on.brightness_slider_value == 73
    assert cmd.first_page_turn_on.affect_lights == Cmd.ALL


def test_next_room_button_shows_next_room(home):
    # HomePage acts on every touch event from this button, press or release,
    # so the TFT reports only one edge for it. Send exactly one: a full
    # press() would advance two rooms and, with two rooms, wrap back.
    home.nextion.touch(BUTTON_NEXT_ROOM, pressed=True)

    home.nextion.wait_for_value("home.room.txt", "Kitchen")
    home.broker.wait_for_subscription(home.panel.client_id, "nspanel/mqttmanager_10.0.2.2/room/2/state")


def test_room_state_from_manager_updates_display(home):
    room = home.manager.rooms[1]
    room.num_ceiling_lights_on = 1
    room.ceiling_lights_dim_level = 40
    home.manager.publish_room(1)

    home.nextion.wait_for_value("home.n_ceiling.val", 40)


def test_room_entities_button_opens_entities_page(home):
    home.nextion.press(BUTTON_ROOM_ENTITIES)

    # page_type 4 in the manager's NSPanelRoomEntitiesPage selects the 4-slot page (GUI_data.hpp).
    home.nextion.wait_for_page("items4")
    home.nextion.wait_for(lambda n: "Office" in [v for k, v in n.components.items() if k.startswith("items4.")],
                          what="entities page header 'Office'")
    home.broker.wait_for_subscription(home.panel.client_id, "nspanel/mqttmanager_10.0.2.2/entity_pages/11/state")
