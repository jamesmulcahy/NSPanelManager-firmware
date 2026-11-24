"""Weather on the screensaver, from the manager's retained weather update."""

import pytest

from nspm_qemu.fake_manager import default_weather
from nspm_qemu.proto import nspanel as pb
from shared_behaviour import SCREENSAVER, lvgl_only, region, settle, wake_to_home


@pytest.fixture
def weather(rig):
    """A panel configured for the weather screensaver, booted and idle on it."""
    rig.manager.config.screensaver_mode = pb.NSPanelConfig.WEATHER_WITH_BACKGROUND
    rig.boot()
    return rig


# ---- The LVGL UI -------------------------------------------------------------------------------
# The LVGL screensaver draws its weather card itself. What the tests check about the weather (it is
# drawn, with all of its forecast, and it waits for the screensaver when it arrives elsewhere) is read
# from the screen's pixels: the weather card (current conditions) and the forecast card below it are empty until weather arrives.
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
