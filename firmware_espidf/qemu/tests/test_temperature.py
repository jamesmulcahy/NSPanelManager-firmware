"""The panel's temperature: nothing is reported before a usable average exists, and the average is
in the configured unit from the start.

QEMU stubs the ADC at a fixed voltage (about 24.6 C, ~76 F), so the numbers are known. The first
average waits for the config, which names the unit and arrives some seconds after boot; the readings
themselves are kept in Celsius, so switching the unit later takes effect at once.

Needs a schema with NSPanelStatusReport.has_temperature: until qemu/nspanelmanager.ref is bumped past
it, point NSPM_PROTO_DIR at a manager checkout's docker/protobuf.
"""

import pytest

from nspm_qemu.proto import nspanel as pb

pytestmark = pytest.mark.skipif(
    "has_temperature" not in pb.NSPanelStatusReport.DESCRIPTOR.fields_by_name,
    reason="the pinned schema has no NSPanelStatusReport.has_temperature",
)

CELSIUS_MIN, CELSIUS_MAX = 20.0, 29.0
FIRST_AVERAGE = r"StatusUpdateManager: Temperature: first average (-?[\d.]+) from (\d+) readings"
UNIT_CHANGED = r"StatusUpdateManager: Temperature: unit or offset changed, average is now (-?\d+\.\d+)"


def fahrenheit(celsius):
    return celsius * 1.8 + 32


def test_first_average_is_in_the_configured_unit_and_reported_as_present(rig):
    rig.manager.config.use_fahrenheit = True
    rig.boot()

    # The config arrives with the first boot, and the average waits for it: it is never in Celsius here.
    match = rig.panel.wait_for_log(FIRST_AVERAGE, timeout=120)
    average = float(match.group(1))
    assert fahrenheit(CELSIUS_MIN) < average < fahrenheit(CELSIUS_MAX), average
    assert int(match.group(2)) >= 5

    report = rig.status_report(timeout=120)
    assert report.has_temperature
    assert fahrenheit(CELSIUS_MIN) < report.temperature < fahrenheit(CELSIUS_MAX), report.temperature


def test_switching_the_unit_changes_the_average_at_once(rig):
    rig.manager.config.use_fahrenheit = False
    rig.boot()
    first = float(rig.panel.wait_for_log(FIRST_AVERAGE, timeout=120).group(1))
    assert CELSIUS_MIN < first < CELSIUS_MAX, first

    rig.manager.config.use_fahrenheit = True
    rig.manager.publish_config()
    switched = float(rig.panel.wait_for_log(UNIT_CHANGED, timeout=120).group(1))
    assert switched == pytest.approx(fahrenheit(first), abs=0.2)
