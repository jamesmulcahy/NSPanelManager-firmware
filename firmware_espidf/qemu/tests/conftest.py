import logging
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from nspm_qemu.rig import DEFAULT_BUILD_DIR, Rig  # noqa: E402

LOG_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "test-logs")


def pytest_configure(config):
    logging.getLogger("broker").setLevel(logging.WARNING)
    build_dir = os.environ.get("NSPM_QEMU_BUILD_DIR", DEFAULT_BUILD_DIR)
    if not os.path.exists(os.path.join(build_dir, "firmware.bin")):
        pytest.exit("no firmware in %s; build it with `pio run -e qemu` (or use qemu/run_tests.sh)" % build_dir, 2)


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    setattr(item, "rep_" + report.when, report)


@pytest.fixture
def rig(request):
    """A fresh emulated panel per test. Its console log is kept under build/test-logs/<test>/."""
    name = re.sub(r"[^\w.-]", "_", request.node.nodeid.split("::", 1)[-1])
    r = Rig(os.path.join(LOG_ROOT, name))
    try:
        yield r
    finally:
        r.stop()
        failed = any(getattr(request.node, "rep_" + w, None) is not None and getattr(request.node, "rep_" + w).failed
                     for w in ("setup", "call"))
        if failed:
            sys.stderr.write("\n---- panel console (last 60 lines) ----\n%s\n" % r.panel.tail(60))
            sys.stderr.write("---- display: page=%r, last commands: %r\n" % (r.nextion.page, r.nextion.commands[-15:]))
        # A crash the test did not wait long enough to notice is still a failure.
        if r.panel.crash and not failed:
            pytest.fail("firmware crashed: %s\n%s" % (r.panel.crash, r.panel.tail(40)))


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
