"""Panel timers on the LVGL UI: start one from the swipe-down menu, watch the engine's wakeups close in
on it, see the alarm when it fires, and find a running one again after a reboot.

Needs NSPM_QEMU_TERMINAL=us (the LVGL UI, the default; touch coordinates are for the US layout), as
test_lvgl_terminal.py does. The coordinates below are from the host simulator's screens
(test/host/lvgl_sim: us-32-menu, us-38-timer-set, us-39-timer-alarm).

The firmware logs what the tests read: TimerManager at INFO for a timer started, fired and restored,
and at DEBUG for each scheduler wakeup ("Wakeup: ..."), which is how the closing-in schedule can be
seen without a clock on the host.
"""

import re
import time

import pytest

from nspm_qemu.fake_nextion import TERMINAL

from test_lvgl_terminal import settle, swipe_down, upright_rgb565, wake_to_home

pytestmark = pytest.mark.skipif(TERMINAL is None, reason="needs the LVGL UI: NSPM_QEMU_TERMINAL=us|eu-on-us (the default)")

US_MENU_TIMERS = (238, 407)
US_ROLLER_X = {"hours": 56, "minutes": 160, "seconds": 264}
# Rows of a roller, as the screen shows it: the selected row is in the middle, and a tap on another row
# selects it. Two rows either side are visible.
ROLLER_ROW_Y = {-2: 102, -1: 145, 0: 188, 1: 231, 2: 274}
US_CHIP_10_MIN = (160, 322)
US_TIMER_START = (233, 373)
US_DISMISS = (160, 418)
US_DISMISS_BOX = (8, 368, 312, 468)  # x0, y0, x1, y1


@pytest.fixture
def lvgl_home(booted):
    if TERMINAL != "us":
        pytest.skip("touch coordinates are for the US layout")
    wake_to_home(booted)
    return booted


def open_timers(rig) -> None:
    """Swipe down, tap Timers: with none running, that opens the set page."""
    mark = rig.panel.mark()
    swipe_down(rig)
    rig.panel.wait_for_log(r"MenuPage: Showing the menu", since=mark, timeout=30)
    settle(rig.nextion)
    mark = rig.panel.mark()
    rig.nextion.tap_screen(*US_MENU_TIMERS)
    rig.panel.wait_for_log(r"TimerPage: Showing the timer set page", since=mark, timeout=30)
    settle(rig.nextion)
    rig.nextion.screenshot("timer-set")


def tap_roller_row(rig, roller: str, row: int) -> None:
    rig.nextion.tap_screen(US_ROLLER_X[roller], ROLLER_ROW_Y[row])
    settle(rig.nextion)  # The roller animates to the row it was given


def set_three_seconds(rig) -> None:
    """The set page opens on 0:05:00. Tap the rows towards 0:00:03: the minutes go 5 -> 3 -> 1 -> 0, the
    seconds 0 -> 2 -> 3, each tap on a visible row."""
    for row in (-2, -2, -1):
        tap_roller_row(rig, "minutes", row)
    for row in (2, 1):
        tap_roller_row(rig, "seconds", row)
    rig.nextion.screenshot("timer-set-0-00-03")


def guest_ms(match):
    """The firmware's own timestamp of a console line ("I (12345) Tag: ..."), or None without one."""
    m = re.search(r"\((\d+)\) ", match.string)
    return int(m.group(1)) if m else None


def dominant_share(rig, box) -> float:
    """How much of `box` of the screen is its most common colour: a filled button covers most of it."""
    import numpy as np
    x0, y0, x1, y1 = box
    region = upright_rgb565(rig.nextion)[y0:y1, x0:x1]
    _, counts = np.unique(region, return_counts=True)
    return float(counts.max()) / region.size


def test_timer_runs_down_fires_shows_the_alarm_and_dismiss_goes_home(lvgl_home):
    rig = lvgl_home
    open_timers(rig)
    set_three_seconds(rig)

    # A screensaver timeout shorter than the alarm test needs to see, set once the set page is ready so that
    # the taps above are not racing it. It counts from the last touch: the Start tap.
    rig.manager.config.screensaver_activation_timeout = 15000
    rig.manager.publish_config()
    time.sleep(2)

    mark = rig.panel.mark()
    rig.nextion.tap_screen(*US_TIMER_START)
    started = rig.panel.wait_for_log(r"TimerManager: Timer (\d+) started for 3 s", since=mark, timeout=30)
    timer_id = started.group(1)

    # The wakeups close in on the deadline: the first comes about halfway, with the timer still running...
    first = rig.panel.wait_for_log(r"TimerManager: Wakeup: 0 fired, soonest has (\d+) ms left, next wake in (\d+) ms", since=mark,
                                   timeout=30)
    assert 0 < int(first.group(1)) < 3000, first.string
    assert 0 < int(first.group(2)) <= int(first.group(1)), first.string
    # ... and the last lands on the deadline, where the timer fires, about three seconds after it started.
    fired = rig.panel.wait_for_log(r"TimerManager: Timer %s \(3 s\) fired\." % timer_id, since=mark, timeout=30)
    rig.panel.wait_for_log(r"TimerManager: Wakeup: 1 fired, no timers left", since=mark, timeout=10)
    if guest_ms(started) is not None and guest_ms(fired) is not None:
        elapsed = guest_ms(fired) - guest_ms(started)
        assert 2900 <= elapsed <= 3600, "fired %d ms after it started" % elapsed

    # The alarm view takes the screen: Timer done, with a large Dismiss button.
    before = upright_rgb565(rig.nextion)
    rig.panel.wait_for_log(r"TimerPage: Showing the timer alarm \(1 done\)", since=mark, timeout=30)
    settle(rig.nextion)
    rig.nextion.screenshot("timer-alarm")
    assert (upright_rgb565(rig.nextion) != before).any()
    assert dominant_share(rig, US_DISMISS_BOX) > 0.6, "no large Dismiss button on the screen"

    # The screensaver timeout runs out while the alarm is showing, and the alarm keeps the screen.
    rig.panel.wait_for_log(r"InterfaceManager: Got sleep event while the timer alarm is showing", since=mark, timeout=90)
    assert not rig.panel.log_lines(r"ScreensaverPage: Showing screensaver", since=mark)
    assert dominant_share(rig, US_DISMISS_BOX) > 0.6
    alarm = upright_rgb565(rig.nextion)

    # Dismiss: nothing else is running, so it goes to the default page (Home).
    mark = rig.panel.mark()
    rig.nextion.tap_screen(*US_DISMISS)
    rig.panel.wait_for_log(r"TimerManager: Dismissed 1 fired timer\(s\)", since=mark, timeout=30)
    rig.panel.wait_for_log(r"HomePage: Updating displayed values", since=mark, timeout=30)
    settle(rig.nextion)
    rig.nextion.screenshot("home-after-dismiss")
    assert (upright_rgb565(rig.nextion) != alarm).mean() > 0.2, "the alarm is still on the screen"
    assert not rig.nextion.terminal.faults


def test_running_timer_is_restored_after_a_reboot(lvgl_home):
    """The wall clock comes from the manager's epoch topic, which the harness can send: a timer started
    with the clock set is saved with its wall-clock deadline, and comes back when the clock is set again
    after the reboot."""
    rig = lvgl_home
    epoch_topic = rig.manager.base + "/status/epoch"

    def send_epoch():
        rig.broker.publish(epoch_topic, str(int(time.time() * 1000)))

    mark = rig.panel.mark()
    send_epoch()
    rig.panel.wait_for_log(r"PanelClock: Clock set from manager", since=mark, timeout=60)

    open_timers(rig)
    mark = rig.panel.mark()
    rig.nextion.tap_screen(*US_CHIP_10_MIN)
    settle(rig.nextion)
    rig.nextion.tap_screen(*US_TIMER_START)
    started = rig.panel.wait_for_log(r"TimerManager: Timer \d+ started for 600 s(.*)", since=mark, timeout=30)
    assert "not saved" not in started.group(1), "the clock was set: the timer should have been saved"
    rig.panel.wait_for_log(r"ConfigManager: Saved \d+ bytes to /littlefs/config.json", since=mark, timeout=120)

    boots, events, mark = rig.panel.boots, rig.broker.event_mark(), rig.panel.mark()
    rig.manager.send_command("reboot")
    rig.panel.wait_for_reboot(boots)
    # The panel subscribes to the epoch topic once the manager has accepted it again; the timer comes back
    # when the clock is set.
    rig.broker.wait_for_subscription(rig.panel.client_id, epoch_topic, timeout=180, since=events)
    send_epoch()
    restored = rig.panel.wait_for_log(r"TimerManager: Timer \d+ restored: 600 s timer with (\d+) s left", since=mark, timeout=60)
    assert 0 < int(restored.group(1)) <= 600, restored.string
    assert not rig.panel.log_lines(r"TimerManager: Dropped", since=mark)
