"""Weather on the screensaver, from the manager's retained weather update."""

import pytest

from nspm_qemu.fake_manager import default_weather
from nspm_qemu.proto import nspanel as pb


@pytest.fixture
def weather(rig):
    """A panel configured for the weather screensaver, booted and idle on it.

    The display runs at a real one's speed, with its ~1 KB input buffer (nextion-tools' flow
    model), from boot: the boot sequence and the forecast after `page screensaver` are the two
    bursts that used to overflow it on a panel.
    """
    rig.nextion.flow_model = True
    rig.manager.config.screensaver_mode = pb.NSPanelConfig.WEATHER_WITH_BACKGROUND
    rig.boot()
    rig.nextion.wait_for_page("screensaver")
    return rig


def test_weather_is_drawn_on_the_screensaver(weather):
    """All of it, with nothing lost to the display's input buffer. Before the write window (#54),
    the forecast overran the buffer while the display was still drawing `page screensaver`: 18-54
    commands lost per boot on Office, and missing weather icons on Master Bedroom."""
    sent = weather.manager.publish_weather()
    n = weather.nextion
    n.wait_for_value("screensaver.curTemp.txt", sent.current_temperature_string)
    try:
        n.wait_for_value("screensaver.forWind5.txt", "10 km/h")  # the last label the update writes
    except TimeoutError:
        if n.lost:
            pytest.fail("forecast not drawn: the display's buffer overflowed (%d time(s)) and lost %d commands, e.g. %r"
                        % (n.overflows, len(n.lost), n.lost[:3]))
        raise
    n.wait_until_idle()
    assert n.overflows == 0 and not n.lost, "display buffer overflowed %d times, lost %r" % (n.overflows, n.lost)
    assert n.get("screensaver.forDay5.txt") == "Fri"
    assert n.get("screensaver.forDay1.txt") == "Mon"
    assert n.get("screensaver.forIcon3.txt") == "c"
    assert n.get("screensaver.curSunset.txt") == "18:20"


def test_weather_that_arrives_on_another_page_waits_for_the_screensaver(weather):
    """Weather that arrives while another page is shown is not drawn over it, and the screensaver
    shows it when it comes back. It used to be rendered anyway: ~2 KB of writes into the page in
    use, and all 25 forecast labels rejected, since they were not page-qualified (#53)."""
    weather.wake_to_home()
    n = weather.nextion
    mark = n.mark()
    sent = default_weather()
    sent.current_temperature_string = "9°"
    sent.forecast_items[4].wind_string = "3 km/h"
    weather.manager.publish_weather(sent)

    # The panel handles MQTT messages in order, so once this room update is drawn the weather
    # before it has been handled too.
    room = weather.manager.rooms[1]
    room.num_ceiling_lights_on, room.ceiling_lights_dim_level = 1, 40
    weather.manager.publish_room(1)
    n.wait_for_value("home.n_ceiling.val", 40)
    assert not [c for c in n.commands[mark:] if "curTemp" in c or "forWind" in c], "weather drawn on the home page"

    n.sleep()
    n.wait_for_page("screensaver")
    n.wait_for_value("screensaver.curTemp.txt", "9°")
    n.wait_for_value("screensaver.forWind5.txt", "3 km/h")
