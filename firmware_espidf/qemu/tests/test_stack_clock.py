"""The panel's wall clock and timezone, as the real NSPanelManager sets them (see lib/PanelClock).

Runs against the dev stack, like test_stack.py. The stack's manager runs on this host,
so the time it sends is the harness's own time.
"""

import re
import subprocess
import sys
import time

import pytest

pytestmark = pytest.mark.stack

CLOCK_SET = r"PanelClock: Clock set from manager to (\d+) ms"
CLOCK_MOVED = r"PanelClock: Clock set from manager, moved by (-?\d+) ms"
TIMEZONE_SET = r"PanelClock: Timezone set from manager to '(.*)'\."
LOCAL_TIME = r"PanelClock: Local time is (.*) at (\d+) s since the Unix epoch\."
LOCAL_TIME_FORMAT = "%Y-%m-%d %H:%M:%S %Z (UTC%z)"  # as PanelClock::_log_local_time() formats it

# The manager sends the time every minute, on its own MQTT (re)connect, and in reply to a
# panel's `online`. A booting panel publishes `online` before register_accept tells it the
# manager's address, so before it has subscribed to the time, and waits for the next minute.
FIRST_TIME_TIMEOUT = 90


def _epoch_message(rig, value_ms: int):
    """The time message the manager sent with this value."""
    return rig.broker.wait_for_message(rig.manager.base + "/status/epoch",
                                       lambda m: m.payload == str(value_ms).encode(), timeout=5)


def test_clock_is_set_from_the_manager(booted):
    accepted_at = time.monotonic()
    set_ms = int(booted.panel.wait_for_log(CLOCK_SET, timeout=FIRST_TIME_TIMEOUT)[1])
    print("clock set %.1f s after boot finished" % (time.monotonic() - accepted_at))

    message = _epoch_message(booted, set_ms)
    assert not message.retain
    assert abs(set_ms - time.time() * 1000) < 10_000


def _host_local_time(tz_posix: str, unix_s: int) -> str:
    """What glibc makes of the rule at that instant. In a child process, so TZ stays unchanged here."""
    code = "import os, sys, time; os.environ['TZ'] = sys.argv[1]; time.tzset(); " \
           "print(time.strftime(sys.argv[2], time.localtime(int(sys.argv[3]))))"
    return subprocess.run([sys.executable, "-c", code, tz_posix, LOCAL_TIME_FORMAT, str(unix_s)],
                          check=True, capture_output=True, text=True).stdout.strip()


def test_local_time_follows_the_managers_timezone(booted):
    """Set the stack's timezone with NSPM_DEV_TZ. The image's own is UTC, which can't tell
    a rule newlib understood from one it fell back to UTC on."""
    tz = booted.manager.config.tz_posix
    assert tz, "the stack's manager sent no tz_posix"
    booted.panel.wait_for_log(TIMEZONE_SET, timeout=30)
    booted.panel.wait_for_log(CLOCK_SET, timeout=FIRST_TIME_TIMEOUT)

    # The panel logs the local time when its clock is first set and when its timezone
    # changes, whichever comes second has both. It may still be on its way.
    deadline = time.monotonic() + 10
    while True:
        lines = booted.panel.log_lines(TIMEZONE_SET + "|" + LOCAL_TIME)
        if re.search(LOCAL_TIME, lines[-1]) or time.monotonic() > deadline:
            break
        time.sleep(0.2)
    assert re.search(LOCAL_TIME, lines[-1]), "no local time logged after the timezone was set: %r" % lines
    assert re.search(TIMEZONE_SET, "\n".join(lines))[1] == tz

    local, unix_s = re.search(LOCAL_TIME, lines[-1]).groups()
    print("tz_posix %r: panel says %s" % (tz, local))
    assert local == _host_local_time(tz, int(unix_s))


def test_clock_keeps_time_between_updates(booted):
    booted.panel.wait_for_log(CLOCK_SET, timeout=FIRST_TIME_TIMEOUT)
    mark = booted.panel.mark()
    moved_ms = int(booted.panel.wait_for_log(CLOCK_MOVED, since=mark, timeout=75)[1])
    print("clock moved by %d ms after a minute" % moved_ms)
    # The update a minute later finds the clock where it left it, give or take the
    # message's delivery time and QEMU's timekeeping.
    assert abs(moved_ms) < 2_000


def test_clock_is_reset_when_the_manager_restarts(booted):
    booted.panel.wait_for_log(CLOCK_SET, timeout=FIRST_TIME_TIMEOUT)
    mark = booted.panel.mark()

    booted.manager.restart()

    booted.panel.wait_for_log(r"Manager is online!", since=mark, timeout=120)
    # Sent when the restarted manager connects to MQTT, so well before the next minute.
    moved_ms = int(booted.panel.wait_for_log(CLOCK_MOVED, since=mark, timeout=30)[1])
    assert abs(moved_ms) < 2_000
    assert not booted.panel.log_lines(r"W \(\d+\) PanelClock|E \(\d+\) PanelClock")
