"""GUI (TFT) updates: the display must end up with exactly the file the manager served.

The upload protocol is the same whatever the display runs, but the emulated display before and
after an update is not, so every case names the TFT the display starts with and the one it ends on:

  hmi-to-hmi            NSPM's own TFT (NSPM_QEMU_TERMINAL=nextion)
  hmi-to-terminal       the migration: a panel on NSPM's TFT is sent the terminal TFT and, once the
                        firmware reboots, starts the LVGL UI on it (NSPM_QEMU_TERMINAL=nextion)
  terminal-to-terminal  a panel on the terminal TFT is sent another build of it (NSPM_QEMU_TERMINAL=us|eu-on-us)

The display is told what a completed upload turns it into (Rig.set_tft(installs=...), see
FakeNextion.installs): from the display's next power cycle it runs the new TFT.
"""

import random
from collections import namedtuple

import pytest

from nspm_qemu.gui_update import check_invariants, run_gui_update
from nspm_qemu.fake_nextion import TERMINAL
from nspm_qemu.proto import nspanel as pb

# Not a multiple of 4096, so the last chunk is a short one.
TFT_SIZE = 200003

# before: the TFT the display starts with; after: what the update installs ("terminal"), or None to
# leave the display running the TFT it had.
Transition = namedtuple("Transition", "before after")

HMI_ONLY = pytest.mark.skipif(TERMINAL is not None, reason="NSPM's TFT only (NSPM_QEMU_TERMINAL=nextion)")
LVGL_ONLY = pytest.mark.skipif(
    TERMINAL is None, reason="needs the LVGL UI: NSPM_QEMU_TERMINAL=us|eu-on-us (the default)")

TRANSITIONS = {
    "hmi-to-hmi": (Transition("hmi", None), HMI_ONLY),
    "hmi-to-terminal": (Transition("hmi", "terminal"), HMI_ONLY),
    "terminal-to-terminal": (Transition("terminal", "terminal"), LVGL_ONLY),
}


def transitions(*names):
    """The `transition` fixture's params for the named transitions, each skipped where its display is not the one running."""
    return [pytest.param(TRANSITIONS[n][0], id=n, marks=TRANSITIONS[n][1]) for n in names]


@pytest.fixture(params=transitions(*TRANSITIONS))
def transition(request):
    return request.param


@pytest.fixture
def rig(make_rig, transition):
    """A panel whose display starts on the transition's TFT, not started yet."""
    r = make_rig(display=transition.before)
    r.transition = transition
    return r


def stage_tft(rig, data: bytes) -> None:
    """Stage a TFT on the manager that the display accepts, and that installs the transition's `after`."""
    rig.set_tft(data, installs=rig.transition.after)


def make_tft(seed: int, size: int = TFT_SIZE) -> bytes:
    # Random bytes make any offset mistake show up as a mismatch, and contain FF FF FF runs.
    return random.Random(seed).randbytes(size)


def check_display_after(rig, result) -> None:
    """After a successful update the panel runs the UI of the TFT it was sent: the display the
    terminal TFT stands for starts a session LVGL draws on, and the panel says which mode it is in."""
    assert result.display_mode is not None, "the status report has no display_mode\n" + result.summary()
    if rig.transition.after is None:
        assert result.display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_NEXTION, result.summary()
        return
    assert result.display_mode == pb.NSPanelStatusReport.DISPLAY_MODE_LVGL, result.summary()
    assert rig.nextion.terminal_profile is not None, "the display still runs NSPM's TFT"
    rig.panel.wait_for_log(r"LvglUi: LVGL is the UI", timeout=120)
    rig.nextion.wait_for_terminal(timeout=120)
    rig.nextion.wait_for_rectangles(1, timeout=120)
    assert not rig.nextion.terminal.faults


@pytest.mark.parametrize("protocol", ["v1.2", "v1.1"])
def test_gui_update_happy_path(rig, protocol):
    rig.panel.panel_config["use_new_upload_protocol"] = protocol == "v1.2"
    rig.boot()
    stage_tft(rig, make_tft(1))

    result = run_gui_update(rig)

    check_invariants(result)
    assert [u.protocol for u in result.uploads] == [protocol]
    # The first GET (no Range) only probes the size; the file then streams in one ranged request.
    assert [d.range for d in result.downloads] == [None, (0, TFT_SIZE)], result.summary()
    check_display_after(rig, result)
