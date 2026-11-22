import logging
import os
import random
import re
import sys
from typing import List

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from nspm_qemu.display_errors import unexpected  # noqa: E402
from nspm_qemu.rig import DEFAULT_BUILD_DIR, Rig  # noqa: E402
from nspm_qemu.stack_manager import Stack  # noqa: E402

LOG_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "test-logs")


def pytest_configure(config):
    config.addinivalue_line("markers", "stack: runs against an NSPanelManager dev stack (needs NSPM_STACK_MQTT)")
    logging.getLogger("broker").setLevel(logging.WARNING)
    config.addinivalue_line("markers", "display_errors_ok: the test expects the display to reject instructions")


# Skips the default local configuration (LVGL UI, the pinned schema) is expected to satisfy.
# A selected test that would skip for one of these reasons aborts the session before any panel
# boots, unless NSPM_QEMU_ALLOW_SKIPS=1 (CI's Nextion legs, which skip the LVGL tests by design).
_MUST_RUN_REASONS = ("needs the LVGL UI", "the schema has no")


@pytest.hookimpl(trylast=True)
def pytest_collection_modifyitems(config, items):
    """Reports skipif skips up front: the ones local runs should not have abort the session."""
    must, expected = {}, {}
    for item in items:
        for mark in item.iter_markers("skipif"):
            # Marker args are conditions already evaluated at import; read them, do not redo them.
            if not mark.args or not mark.args[0]:
                continue
            reason = mark.kwargs.get("reason", "")
            # Asking for the Nextion TFT is asking not to run the LVGL tests.
            asked_for_nextion = os.environ.get("NSPM_QEMU_TERMINAL") == "nextion"
            bucket = must if reason.startswith(_MUST_RUN_REASONS) and not (
                asked_for_nextion and reason.startswith("needs the LVGL UI")) else expected
            bucket.setdefault(reason, []).append(item.nodeid)
        if item.get_closest_marker("stack") and not os.environ.get("NSPM_STACK_MQTT"):
            expected.setdefault("no dev stack; set NSPM_STACK_MQTT=host:port", []).append(item.nodeid)
    for reason, ids in expected.items():
        sys.stderr.write("will skip %d test(s): %s\n" % (len(ids), reason))
    if not must or os.environ.get("NSPM_QEMU_ALLOW_SKIPS") == "1":
        for reason, ids in must.items():
            sys.stderr.write("will skip %d test(s): %s\n" % (len(ids), reason))
        return
    lines = ["%d selected test(s) would be skipped for an environment reason, so nothing was booted:" %
             sum(len(v) for v in must.values())]
    for reason, ids in must.items():
        lines.append("  %s:" % reason)
        lines += ["    " + i for i in ids[:10]] + (["    ... and %d more" % (len(ids) - 10)] if len(ids) > 10 else [])
    lines.append("Fix the environment (NSPM_QEMU_TERMINAL=us, a schema with the messages: NSPM_PROTO_DIR), "
                 "deselect the tests with -k, or set NSPM_QEMU_ALLOW_SKIPS=1 to run anyway.")
    pytest.exit("\n".join(lines), 2)


def _require_firmware():
    # Checked when a test needs a panel, so tests that need none (test_gui_data) run without.
    build_dir = os.environ.get("NSPM_QEMU_BUILD_DIR", DEFAULT_BUILD_DIR)
    if not os.path.exists(os.path.join(build_dir, "firmware.bin")):
        pytest.exit("no firmware in %s; build it with `pio run -e qemu` (or use qemu/run_tests.sh)" % build_dir, 2)


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    setattr(item, "rep_" + report.when, report)


@pytest.fixture
def make_rig(request):
    """Builds fresh emulated panels, stopped at the end of the test. Each one's console log is
    kept under build/test-logs/<test>/[<n>/], with a PNG of every page the test waited for and of
    the screen when it ended or failed (screens/).

    The test fails if a display rejected an instruction (0x1A) that display_errors.KNOWN does
    not list: on a real panel that is a label never drawn or a picture never shown, and only a
    DEBUG log line says why. Mark a test display_errors_ok if it means to.

    Tests marked `stack` get panels on the dev stack named by NSPM_STACK_MQTT (see
    nspm_qemu/stack_manager.py), and are skipped without one. There every panel gets a
    random MAC and its own hostname, so it is new to the stack, and panels from earlier
    runs or other tests never collide with it."""
    _require_firmware()
    name = re.sub(r"[^\w.-]", "_", request.node.nodeid.split("::", 1)[-1])
    stack = None
    if request.node.get_closest_marker("stack"):
        stack = Stack.from_env()
        if stack is None:
            pytest.skip("no dev stack; set NSPM_STACK_MQTT=host:port")
    rigs: List[Rig] = []

    def make(display=None) -> Rig:
        workdir = os.path.join(LOG_ROOT, name, str(len(rigs))) if rigs else os.path.join(LOG_ROOT, name)
        kwargs = {}
        if stack is not None:
            # Espressif's OUI, as on a real panel.
            suffix = [random.randrange(256) for _ in range(3)]
            kwargs["mac"] = "C0:49:EF:" + ":".join("%02X" % b for b in suffix)
            kwargs["hostname"] = "qemu-" + "".join("%02x" % b for b in suffix)
        if display is not None:
            # "hmi", "terminal", "us" or "eu-on-us": the TFT the display starts with, in place of NSPM_QEMU_TERMINAL's.
            kwargs["display"] = display
        rigs.append(Rig(workdir, stack=stack, **kwargs))
        return rigs[-1]

    try:
        yield make
    finally:
        failed = any(getattr(request.node, "rep_" + w, None) is not None and getattr(request.node, "rep_" + w).failed
                     for w in ("setup", "call"))
        crashed, rejected = [], []
        for i, r in enumerate(rigs):
            label = " %d (%s)" % (i, r.mac) if len(rigs) > 1 else ""
            shot = r.nextion.screenshot("failure" if failed else "end")
            if failed:
                # The panel's log as it reached MQTT: a second copy, independent of the serial console.
                with open(os.path.join(r.panel.workdir, "mqtt_log.txt"), "wb") as f:
                    for msg in list(getattr(r.broker, "messages", [])):
                        if msg.topic == "nspanel/%s/log" % r.mac:
                            f.write(msg.payload if msg.payload.endswith(b"\n") else msg.payload + b"\n")
                cpus = r.panel.dump_cpus(os.path.join(r.panel.workdir, "cpus.txt"))
                if cpus:
                    sys.stderr.write("\n---- what each CPU%s was doing: %s\n" % (label, cpus))
            if failed and shot:
                sys.stderr.write("\n---- the screen%s when it failed: %s\n" % (label, shot))
            r.stop()
            if failed:
                sys.stderr.write("\n---- panel%s console (last 60 lines) ----\n%s\n" % (label, r.panel.tail(60)))
                sys.stderr.write("---- display: page=%r, last commands: %r\n" % (r.nextion.page, r.nextion.commands[-15:]))
            if r.panel.crash:
                crashed.append("panel%s: %s\n%s" % (label, r.panel.crash, r.panel.tail(40)))
            rejected += ["  0x%02X  %s%s" % (code, text, label) for text, code in unexpected(r.nextion.errors)]
        # A crash the test did not wait long enough to notice is still a failure.
        if crashed and not failed:
            pytest.fail("firmware crashed: " + "\n".join(crashed))
        if rejected and not failed and request.node.get_closest_marker("display_errors_ok") is None:
            pytest.fail("the display rejected %d instruction(s) the firmware sent:\n%s" % (len(rejected), "\n".join(rejected)))


@pytest.fixture
def rig(make_rig):
    """A fresh emulated panel, not started yet."""
    return make_rig()


@pytest.fixture
def booted(rig):
    """A panel that has registered, received config, and is idle on its screensaver."""
    rig.boot()
    return rig


@pytest.fixture
def home(booted):
    """A panel woken from its screensaver to the home page."""
    booted.wake_to_home()
    return booted
