"""GUI (TFT) updates: the display must end up with exactly the file the manager served."""

import random

import pytest

from nspm_qemu.gui_update import check_invariants, run_gui_update

# Not a multiple of 4096, so the last chunk is a short one.
TFT_SIZE = 200003


def make_tft(seed: int, size: int = TFT_SIZE) -> bytes:
    # Random bytes make any offset mistake show up as a mismatch, and contain FF FF FF runs.
    return random.Random(seed).randbytes(size)


@pytest.mark.parametrize("protocol", ["v1.2", "v1.1"])
def test_gui_update_happy_path(rig, protocol):
    rig.panel.panel_config["use_new_upload_protocol"] = protocol == "v1.2"
    rig.boot()
    rig.set_tft(make_tft(1))

    result = run_gui_update(rig)

    check_invariants(result)
    assert [u.protocol for u in result.uploads] == [protocol]
    # The first GET (no Range) only probes the size; the file then streams in one ranged request.
    assert [d.range for d in result.downloads] == [None, (0, TFT_SIZE)], result.summary()
