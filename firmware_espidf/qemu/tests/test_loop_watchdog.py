"""The loop_watchdog command: off by default, switched at runtime, kept across reboots."""

import json
import time


def send(rig, **payload):
    rig.broker.publish("nspanel/%s/command" % rig.mac, json.dumps({"command": "loop_watchdog", **payload}))


def test_loop_watchdog_is_off_by_default_and_switches_live(booted):
    rig = booted
    assert not rig.panel.log_lines(r"LoopWatchdog: Enabled")

    mark = rig.panel.mark()
    send(rig, enable=True)
    rig.panel.wait_for_log(r"UpdateManager: loop_watchdog set to true", since=mark, timeout=30)
    rig.panel.wait_for_log(r"LoopWatchdog: Enabled\.", since=mark, timeout=30)

    # A repeat does nothing: no second enable, no second config save.
    mark = rig.panel.mark()
    send(rig, enable=True)
    rig.panel.wait_for_log(r"loop_watchdog is already true", since=mark, timeout=30)
    assert not rig.panel.log_lines(r"LoopWatchdog: Enabled", since=mark)

    # Disabling makes the task exit by itself (within one probe interval).
    mark = rig.panel.mark()
    send(rig, enable=False)
    rig.panel.wait_for_log(r"LoopWatchdog: Disabled", since=mark, timeout=30)
    rig.panel.wait_for_log(r"LoopWatchdog: Watch task stopped\.", since=mark, timeout=30)

    # Enabling again starts a task, with no stale stall reported.
    mark = rig.panel.mark()
    send(rig, enable=True)
    rig.panel.wait_for_log(r"LoopWatchdog: Enabled\.", since=mark, timeout=30)
    time.sleep(6)
    assert not rig.panel.log_lines(r"LoopWatchdog: Default event loop has not dispatched", since=mark)


def test_loop_watchdog_setting_survives_a_reboot(booted):
    rig = booted
    mark = rig.panel.mark()
    send(rig, enable=True)
    rig.panel.wait_for_log(r"LoopWatchdog: Enabled\.", since=mark, timeout=30)

    boots, mark = rig.panel.boots, rig.panel.mark()
    rig.manager.send_command("reboot")
    rig.panel.wait_for_reboot(boots)
    rig.panel.wait_for_log(r"LoopWatchdog: Enabled\.", since=mark, timeout=120)
