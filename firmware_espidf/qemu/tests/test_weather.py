"""Weather on the screensaver, from the manager's retained weather update."""

import pytest

from nspm_qemu.fake_manager import default_weather
from nspm_qemu.fake_nextion import TERMINAL
from nspm_qemu.proto import nspanel as pb
from shared_behaviour import SCREENSAVER, lvgl_only, nextion_only, region, settle, wake_to_home


@pytest.fixture
def weather(rig):
    """A panel configured for the weather screensaver, booted and idle on it.

    The display runs at a real one's speed, with its ~1 KB input buffer (nextion-tools' flow
    model), from boot: the boot sequence and the forecast after `page screensaver` are the two
    bursts that used to overflow it on a panel.
    """
    if TERMINAL is None:
        rig.nextion.flow_model = True
    rig.manager.config.screensaver_mode = pb.NSPanelConfig.WEATHER_WITH_BACKGROUND
    rig.boot()
    if TERMINAL is None:
        rig.nextion.wait_for_page("screensaver")
    return rig


@nextion_only
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


@nextion_only
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


# ---- The LVGL UI -------------------------------------------------------------------------------
# The LVGL screensaver draws its weather card itself: there is no display buffer to overflow and no
# per-label commands to lose, so what the Nextion tests check about the write window has no LVGL
# counterpart. What they check about the weather itself (it is drawn, with all of its forecast, and
# it waits for the screensaver when it arrives elsewhere) is read from the screen's pixels: the
# weather card (current conditions) and the forecast card below it are empty until weather arrives.
# The top card (the clock) is left out, as it changes with the time.

WEATHER_BOX = (8, 166, 312, 284)    # current conditions: icon, temperature, max/min, rain, wind, sun
FORECAST_BOX = (8, 292, 312, 472)   # five days: day, icon, max/min, rain, wind
TEMPERATURE_BOX = (90, 180, 190, 224)


def drawn(nextion, box, blank):
    return (region(nextion, box) != blank).any()


@lvgl_only
def test_weather_is_drawn_on_the_screensaver_on_lvgl(weather):
    """All of it: the current conditions and every day of the forecast."""
    n = weather.nextion
    settle(n)
    blank_weather, blank_forecast = region(n, WEATHER_BOX), region(n, FORECAST_BOX)
    weather.manager.publish_weather()

    n.wait_for(lambda n: drawn(n, FORECAST_BOX, blank_forecast), timeout=60, what="the forecast drawn")
    settle(n)
    assert drawn(n, WEATHER_BOX, blank_weather)
    n.screenshot("weather")
    # Every forecast row has something drawn in it (five rows, each ~31 px tall, in the card's lower 160 px).
    for i in range(5):
        row = (8, 300 + 31 * i, 312, 300 + 31 * (i + 1))
        assert (region(n, row) != region(n, row)[0, 0]).any(), "forecast day %d is blank" % (i + 1)
    assert not n.terminal.faults


@lvgl_only
def test_weather_that_arrives_on_another_page_waits_for_the_screensaver_on_lvgl(weather):
    """Weather that arrives while another page is shown is not drawn over it, and the screensaver
    shows it when it comes back. (The LVGL pages are separate views, so "not drawn over the home
    page" is the home page staying as it is while the update arrives.)"""
    n = weather.nextion
    sent = default_weather()
    sent.current_temperature_string = "9°"
    sent.forecast_items[4].wind_string = "3 km/h"
    # A control: this weather on the screensaver itself, and then other weather in its place, so that
    # the screensaver is not blank and not the weather the test is about to send from the home page.
    settle(n)
    blank = region(n, FORECAST_BOX)
    weather.manager.publish_weather(sent)
    n.wait_for(lambda n: drawn(n, FORECAST_BOX, blank), timeout=60, what="the control weather drawn")
    settle(n)
    expected_weather, expected_forecast = region(n, WEATHER_BOX), region(n, FORECAST_BOX)
    weather.manager.publish_weather()
    n.wait_for(lambda n: (region(n, WEATHER_BOX) != expected_weather).any(), timeout=60, what="the other weather drawn")
    settle(n)

    wake_to_home(weather)
    settle(n)
    mark = weather.panel.mark()
    tile = region(n, (8, 60, 156, 198))
    weather.manager.publish_weather(sent)
    # The panel handles MQTT messages in order, so once this room update is drawn the weather
    # before it has been handled too.
    room = weather.manager.rooms[1]
    room.num_ceiling_lights_on, room.ceiling_lights_dim_level = 1, 40
    weather.manager.publish_room(1)
    n.wait_for(lambda n: (region(n, (8, 60, 156, 198)) != tile).any(), timeout=30, what="the ceiling tile redrawn")
    # The firmware's own account: it kept the weather and rendered none of it.
    assert not weather.panel.log_lines(r"ScreensaverPage: Updating (forecast|screensaver page data)", since=mark), \
        "weather rendered while the home page was shown"
    settle(n)

    # The screen goes off: the screensaver comes back with the weather that arrived meanwhile.
    log = weather.panel.mark()
    weather.broker.publish("nspanel/%s/screen_cmd" % weather.mac, "0")
    weather.panel.wait_for_log(SCREENSAVER, since=log, timeout=30)
    n.wait_for(lambda n: (region(n, WEATHER_BOX) == expected_weather).all() and (region(n, FORECAST_BOX) == expected_forecast).all(),
               timeout=60, what="the screensaver showing the weather that arrived on the home page")
