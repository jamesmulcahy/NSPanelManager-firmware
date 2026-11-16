"""The screensaver's energy card (power as a cost per day, lights on), on the LVGL UI.

The manager publishes NSPanelEnergyUpdate retained on status/energy; the panel decodes it on the GUI
task and logs "Energy update: ...", and the card takes its turn in the rotation ("Screensaver card:
energy (4 s)"). The pixels are checked on the host (test/host/lvgl_sim, test/host/energy_format).

Needs NSPM_QEMU_TERMINAL=us|eu-on-us (the LVGL UI), and a schema that has NSPanelEnergyUpdate:
until qemu/nspanelmanager.ref is bumped past the manager's energy card, point NSPM_PROTO_DIR at a
manager checkout's docker/protobuf.
"""

import time

import pytest

from nspm_qemu.fake_nextion import TERMINAL
from nspm_qemu.proto import nspanel as pb

from test_screensaver_cards import configure, turns, wait_for_turns

pytestmark = [
    pytest.mark.skipif(TERMINAL is None, reason="needs NSPM_QEMU_TERMINAL=us|eu-on-us"),
    pytest.mark.skipif(not hasattr(pb, "NSPanelEnergyUpdate"), reason="the pinned schema has no NSPanelEnergyUpdate"),
]

ENERGY_LOG = r"ScreensaverPage: Energy update: "


def energy_update(watts=3580, cost=9.38, lights_on=6, alert=False):
    available = pb.NSPanelEnergyUpdate.AVAILABLE
    return pb.NSPanelEnergyUpdate(
        power=available,
        power_watts=watts,
        cost=available,
        cost_per_day=cost,
        currency="USD",
        currency_symbol="$",
        lights=available,
        lights_on=lights_on,
        lights_alert=alert,
        generated_at=int(time.time()),
    )


def publish_energy(rig, update_or_bytes):
    payload = update_or_bytes if isinstance(update_or_bytes, bytes) else update_or_bytes.SerializeToString()
    rig.broker.publish(rig.manager.base + "/status/energy", payload, retain=True)


def energy_cards():
    return ((pb.NSPanelConfig.SCREENSAVER_CARD_CALENDAR, 5), (pb.NSPanelConfig.SCREENSAVER_CARD_ENERGY, 4))


def test_energy_card_rotates_after_the_calendar_each_for_its_own_time(rig):
    configure(rig, cards=energy_cards())
    publish_energy(rig, energy_update())
    rig.boot()
    rig.panel.wait_for_log(ENERGY_LOG + r"power 2 \(3580 W\), cost 2 \(938 cents USD/day\), lights 2 \(6, alert: no\)", timeout=60)

    seen = wait_for_turns(rig, 4, since=0)
    assert seen[:4] == [("weather", 3), ("calendar", 5), ("energy", 4), ("weather", 3)], seen
    assert not rig.nextion.terminal.faults


def test_no_energy_update_yet_still_rotates_in(rig):
    configure(rig, cards=((pb.NSPanelConfig.SCREENSAVER_CARD_ENERGY, 4),))
    rig.boot()
    seen = wait_for_turns(rig, 2, since=0)
    assert ("energy", 4) in seen, seen
    assert not rig.panel.log_lines(ENERGY_LOG)


def test_a_newer_energy_update_is_decoded_while_rotating(rig):
    configure(rig, cards=((pb.NSPanelConfig.SCREENSAVER_CARD_ENERGY, 4),))
    publish_energy(rig, energy_update())
    rig.boot()
    rig.panel.wait_for_log(ENERGY_LOG + r".*lights 2 \(6, alert: no\)", timeout=60)
    mark = rig.panel.mark()
    publish_energy(rig, energy_update(watts=-400, cost=-1.05, lights_on=16, alert=True))
    rig.panel.wait_for_log(ENERGY_LOG + r"power 2 \(-400 W\), cost 2 \(-105 cents USD/day\), lights 2 \(16, alert: yes\)", timeout=60, since=mark)
    assert not rig.nextion.terminal.faults


def test_parts_that_are_not_set_up_decode(rig):
    configure(rig, cards=((pb.NSPanelConfig.SCREENSAVER_CARD_ENERGY, 4),))
    update = pb.NSPanelEnergyUpdate(lights=pb.NSPanelEnergyUpdate.AVAILABLE, lights_on=0, generated_at=int(time.time()))
    publish_energy(rig, update)
    rig.boot()
    rig.panel.wait_for_log(ENERGY_LOG + r"power 0 \(0 W\), cost 0 \(0 cents /day\), lights 2 \(0, alert: no\)", timeout=60)
    seen = wait_for_turns(rig, 2, since=0)
    assert ("energy", 4) in seen, seen


def test_a_corrupt_energy_update_keeps_the_previous_one(rig):
    configure(rig, cards=((pb.NSPanelConfig.SCREENSAVER_CARD_ENERGY, 4),))
    publish_energy(rig, energy_update())
    rig.boot()
    rig.panel.wait_for_log(ENERGY_LOG, timeout=60)
    mark = rig.panel.mark()
    publish_energy(rig, b"\xff\xff\xff\xff")
    rig.panel.wait_for_log(r"Got an energy update but failed to decode it\. Keeping the previous one\.", timeout=60, since=mark)
    seen = wait_for_turns(rig, 2, since=mark)
    assert ("energy", 4) in seen, seen
    rig.panel.check_alive()


def test_no_screensaver_shows_no_energy_card(rig):
    configure(rig, mode=pb.NSPanelConfig.NO_SCREENSAVER, cards=((pb.NSPanelConfig.SCREENSAVER_CARD_ENERGY, 4),))
    publish_energy(rig, energy_update())
    rig.boot()
    time.sleep(12)  # three of the card's periods
    assert turns(rig, 0) == []
    rig.panel.check_alive()
