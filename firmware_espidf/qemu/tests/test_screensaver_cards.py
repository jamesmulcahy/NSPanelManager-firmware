"""The screensaver's rotating cards (weather, then the calendar), on the LVGL UI.

The manager's config lists the extra cards, each with its own time, and the base card's time; the
calendar comes from its own retained topic, status/calendar. The view turns to the next card on a
timer and ScreensaverPage logs each turn ("Screensaver card: calendar (5 s)"), which is what these
tests read: the pixels of a card are checked on the host (test/host/lvgl_sim).

Needs NSPM_QEMU_TERMINAL=us|eu-on-us (the LVGL UI), and a schema that has NSPanelCalendarUpdate:
until qemu/nspanelmanager.ref is bumped past the manager's screensaver cards, point NSPM_PROTO_DIR at
a manager checkout's docker/protobuf.
"""

import re
import time

import pytest

from nspm_qemu.fake_nextion import TERMINAL
from nspm_qemu.proto import nspanel as pb

pytestmark = [
    pytest.mark.skipif(TERMINAL is None, reason="needs NSPM_QEMU_TERMINAL=us|eu-on-us"),
    pytest.mark.skipif(not hasattr(pb, "NSPanelCalendarUpdate"), reason="the pinned schema has no NSPanelCalendarUpdate"),
]

CARD = r"ScreensaverPage: Screensaver card: (\w+) \((\d+) s\)"


def calendar_update(events=(), lookahead_hours=48):
    now = int(time.time())
    update = pb.NSPanelCalendarUpdate(lookahead_hours=lookahead_hours, generated_at=now)
    for i, (summary, when) in enumerate(events):
        update.events.add(summary=summary, start=now + 3600 * (i + 1), end=now + 3600 * (i + 2), when_string=when)
    return update


def publish_calendar(rig, update_or_bytes):
    payload = update_or_bytes if isinstance(update_or_bytes, bytes) else update_or_bytes.SerializeToString()
    rig.broker.publish(rig.manager.base + "/status/calendar", payload, retain=True)


def configure(rig, mode=pb.NSPanelConfig.WEATHER_WITH_BACKGROUND, base_seconds=3, cards=None):
    # Default resolved here, not in the signature: with an older pinned schema the enum doesn't exist,
    # and a signature default would fail collection before the skipif marks above can skip the module.
    if cards is None:
        cards = ((pb.NSPanelConfig.SCREENSAVER_CARD_CALENDAR, 5),)
    config = rig.manager.config
    config.screensaver_mode = mode
    config.screensaver_base_card_seconds = base_seconds
    for card_type, seconds in cards:
        config.screensaver_cards.add(type=card_type, seconds=seconds)


def turns(rig, since):
    return [(m.group(1), int(m.group(2))) for m in map(re.compile(CARD).search, rig.panel.log_lines(CARD, since=since)) if m]


def wait_for_turns(rig, count, since, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        seen = turns(rig, since)
        if len(seen) >= count:
            return seen
        rig.panel.check_alive()
        time.sleep(0.5)
    raise TimeoutError("only %r of %d card turns within %d s" % (turns(rig, since), count, timeout))


def test_calendar_card_rotates_with_weather_each_for_its_own_time(rig):
    configure(rig)
    publish_calendar(rig, calendar_update([("Dentist", "Today 3:00 PM"), ("Soccer", "Tomorrow 9:00 AM")]))
    rig.boot()
    rig.panel.wait_for_log(r"ScreensaverPage: Calendar update: 2 event\(s\), looking 48 h ahead", timeout=60)

    seen = wait_for_turns(rig, 4, since=0)
    assert seen[:4] == [("weather", 3), ("calendar", 5), ("weather", 3), ("calendar", 5)], seen
    assert not rig.nextion.terminal.faults


def test_empty_calendar_still_rotates_in(rig):
    configure(rig)
    publish_calendar(rig, calendar_update())  # no events: still a non-empty payload (generated_at)
    rig.boot()
    rig.panel.wait_for_log(r"ScreensaverPage: Calendar update: 0 event\(s\)", timeout=60)

    seen = wait_for_turns(rig, 3, since=0)
    assert ("calendar", 5) in seen, seen
    assert not rig.nextion.terminal.faults


def test_no_calendar_update_yet_still_rotates_in(rig):
    configure(rig)
    rig.boot()
    seen = wait_for_turns(rig, 2, since=0)
    assert ("calendar", 5) in seen, seen


def test_a_newer_calendar_update_is_decoded_while_rotating(rig):
    configure(rig)
    publish_calendar(rig, calendar_update([("First", "Today 1:00 PM")]))
    rig.boot()
    rig.panel.wait_for_log(r"Calendar update: 1 event\(s\)", timeout=60)
    mark = rig.panel.mark()
    publish_calendar(rig, calendar_update([("A", "Today 1:00 PM"), ("B", "Today 2:00 PM"), ("C", "Today 3:00 PM")], lookahead_hours=24))
    rig.panel.wait_for_log(r"Calendar update: 3 event\(s\), looking 24 h ahead", timeout=60, since=mark)
    assert not rig.nextion.terminal.faults


def test_config_change_of_cards_takes_effect_while_showing(rig):
    configure(rig, cards=())
    publish_calendar(rig, calendar_update([("Dentist", "Today 3:00 PM")]))
    rig.boot()
    time.sleep(8)  # two of the weather's periods
    assert {card for card, _ in turns(rig, 0)} <= {"weather"}, "no extra card: the weather stays"

    mark = rig.panel.mark()
    config = rig.manager.config
    config.screensaver_cards.add(type=pb.NSPanelConfig.SCREENSAVER_CARD_CALENDAR, seconds=4)
    rig.manager.publish_config()
    seen = wait_for_turns(rig, 2, since=mark)
    assert ("calendar", 4) in seen, seen


def test_no_screensaver_does_not_rotate(rig):
    configure(rig, mode=pb.NSPanelConfig.NO_SCREENSAVER)
    publish_calendar(rig, calendar_update([("Dentist", "Today 3:00 PM")]))
    rig.boot()
    time.sleep(20)  # four of the calendar's periods
    assert turns(rig, 0) == []
    rig.panel.check_alive()


def test_oversized_calendar_update_is_dropped_cleanly(rig):
    configure(rig)
    rig.boot()
    mark = rig.panel.mark()
    big = calendar_update([("x" * 60, "Today 3:00 PM")] * 100)  # ~8 KB
    payload = big.SerializeToString()
    assert len(payload) > 4096
    publish_calendar(rig, payload)
    rig.panel.wait_for_log(r"MqttManager: Dropping \d+ byte message on .*/status/calendar", timeout=60, since=mark)
    assert not rig.panel.log_lines(r"Calendar update:", since=mark)

    # And the next, valid one is taken as usual.
    publish_calendar(rig, calendar_update([("Dentist", "Today 3:00 PM")]))
    rig.panel.wait_for_log(r"Calendar update: 1 event\(s\)", timeout=60, since=mark)
    rig.panel.check_alive()
