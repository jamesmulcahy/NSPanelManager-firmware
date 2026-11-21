"""Library: runs a firmware update on a Rig, and reads LittleFS back out of the flash image
afterwards.

The setup web UI is part of the firmware image, so a firmware update replaces the app and nothing
else: the LittleFS partition holds only the panel's own /littlefs/config.json, and an update must
neither read from nor write to it beyond the checksums it stores itself. What these helpers let a
test ask is whether the panel came back on the new firmware, registered under its own name and
with its settings.
"""

import json
import os
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from .image import read_partitions
from .update_server import Request

# The partition holding LittleFS (lib/LittleFSManager mounts the one labelled "spiffs").
FS_PARTITION = "spiffs"

# Budget for one update: a 1.5 MB download and a reboot.
UPDATE_TIMEOUT = 420
# From a reboot to the first register_request (see rig.BOOT_TIMEOUT).
REGISTER_TIMEOUT = 180
# A boot with no reboot for this long after it registered is the last one. Only an update
# installed by a firmware that recorded no checksum reboots a second time, about 1 s after
# registering.
SETTLED = 25


@dataclass
class OtaResult:
    rebooted: bool
    reboots: int  # how many times the panel rebooted between the update command and settling
    registration: Optional[dict]  # the first register_request after the update, if any
    files: Dict[str, bytes]  # what LittleFS holds after the update (read back from flash)
    mounted: bool  # whether the LittleFS partition on flash could be read at all
    requests: List[Request]  # everything the panel asked the update server for
    console: List[str]  # the panel's UpdateManager/LittleFS lines during the update
    elapsed: float
    stalled: Optional[str] = None
    extra: dict = field(default_factory=dict)

    @property
    def config(self) -> Optional[dict]:
        raw = self.files.get("config.json")
        if raw is None:
            return None
        try:
            return json.loads(raw)
        except ValueError:
            return None

    def summary(self) -> str:
        reg = self.registration or {}
        lines = ["rebooted=%s (%d), registered as %r with md5_data_file=%r md5_firmware=%r, littlefs %s (%s), %.0fs%s" % (
            self.rebooted, self.reboots, reg.get("friendly_name"), reg.get("md5_data_file"), reg.get("md5_firmware"),
            "readable" if self.mounted else "UNREADABLE", ", ".join(sorted(self.files)) or "empty",
            self.elapsed, ", gave up: " + self.stalled if self.stalled else "")]
        for r in self.requests:
            lines.append("  GET %s -> %s sent=%d/%d %s" % (r.path, r.status, r.sent, r.length, r.outcome))
        lines.append("  panel:")
        lines += ["    " + c for c in self.console[-40:]]
        return "\n".join(lines)


def partition(rig, name: str = FS_PARTITION) -> Dict[str, int]:
    return read_partitions(os.path.join(rig.panel.project_dir, "partitions.csv"))[name]


def read_littlefs(rig) -> Optional[Dict[str, bytes]]:
    """The files on the panel's LittleFS partition, from its flash image; None if it does not mount.

    Uses littlefs-python, not mklittlefs: PlatformIO's mklittlefs (0.2.3) carries an
    older LittleFS that silently misses files the firmware wrote (config.json among
    them). Only call this right after a reboot: QEMU is restarted on every firmware
    reset, so the flash file is complete then.
    """
    import littlefs  # littlefs-python

    part = partition(rig)
    with open(rig.panel._flash, "rb") as f:
        f.seek(part["offset"])
        raw = f.read(part["size"])
    # Geometry as lib/LittleFSManager mounts it (sdkconfig: CONFIG_LITTLEFS_PAGE_SIZE=256).
    fs = littlefs.LittleFS(block_size=4096, block_count=part["size"] // 4096, read_size=256, prog_size=256,
                           mount=False)
    fs.context.buffer = bytearray(raw)
    try:
        fs.mount()
    except Exception:  # littlefs.errors.LittleFSError, but any failure means "does not mount"
        return None
    files = {}
    for root, _dirs, names in fs.walk("/"):
        for name in names:
            path = root.rstrip("/") + "/" + name
            with fs.open(path, "rb") as f:
                files[path.lstrip("/")] = f.read()
    return files


def start_firmware_update(rig) -> None:
    """Press "update firmware" in the manager, once the panel is listening for commands."""
    rig.broker.wait_for_subscription(rig.panel.client_id, "nspanel/%s/command" % rig.mac, timeout=60)
    rig.manager.send_command("firmware_update")


def _wait_until_settled(rig, boots_before: int, registrations_before: int, deadline: float) -> dict:
    """Wait for the panel to reboot, possibly more than once, then register and stay up.

    Returns the register_request of the last boot. A panel that lost its
    settings boots into its fallback AP and never registers, so each boot gets
    REGISTER_TIMEOUT to do so.
    """
    rig.panel.wait_until(lambda: rig.panel.boots > boots_before, timeout=max(1, deadline - time.monotonic()),
                         what="reboot after the update")
    boot, boot_started = rig.panel.boots, time.monotonic()
    # Registrations so far, and how many there were when the current boot began.
    at_boot = registrations_before
    registered_at = None
    while True:
        rig.panel.check_alive()
        now = time.monotonic()
        if rig.panel.boots != boot:
            boot, boot_started, registered_at = rig.panel.boots, now, None
            at_boot = rig.manager.registration_mark()
            # A registration sent before QEMU restarted belongs to the old boot; anything
            # after `at_boot` arrived once the new boot was under way.
        elif registered_at is None and rig.manager.registration_mark() > at_boot:
            registered_at = now
        elif registered_at is not None and now - registered_at >= SETTLED:
            return rig.manager.register_requests[-1]
        if registered_at is None and now - boot_started > REGISTER_TIMEOUT:
            raise TimeoutError("panel did not register within %.0fs of boot %d" % (REGISTER_TIMEOUT, boot))
        if now > deadline:
            raise TimeoutError("update did not settle within the budget (boot %d, registered=%s)" % (
                boot, registered_at is not None))
        time.sleep(0.2)


def run_update(rig, timeout: float = UPDATE_TIMEOUT) -> OtaResult:
    """Start a firmware update and wait for the panel to reboot onto it and register again.

    A firmware update reboots once, into the new image (twice if the firmware that installed it
    recorded no checksum), so this waits until a registration after the last reboot. Never fails
    by itself; a hang comes back with `stalled` set.
    """
    first_request = rig.update_server.mark()
    console = rig.panel.mark()
    boots = rig.panel.boots
    t0 = time.monotonic()
    start_firmware_update(rig)

    stalled = None
    registration = None
    try:
        registration = _wait_until_settled(rig, boots, rig.manager.registration_mark(), t0 + timeout)
    except TimeoutError as e:
        stalled = str(e)
    rebooted = rig.panel.boots > boots

    files = read_littlefs(rig) if rebooted else None
    return OtaResult(
        rebooted=rebooted,
        reboots=rig.panel.boots - boots,
        registration=registration,
        files=files or {},
        mounted=files is not None,
        requests=list(rig.update_server.requests[first_request:]),
        console=[line for line in rig.panel.lines[console:]
                 if any(t in line for t in ("UpdateManager", "LittleFS", "ConfigManager", "Main:"))],
        elapsed=time.monotonic() - t0,
        stalled=stalled,
    )


def check_firmware_updated(rig, result: OtaResult) -> None:
    """The panel came back on the new firmware, registered under its own name, with its settings on flash."""
    info = "\n" + result.summary()
    assert result.stalled is None, "update did not finish: %s%s" % (result.stalled, info)
    assert result.rebooted, "panel did not reboot%s" % info
    # The checksum recorded when flashing is stored on the first boot of the new image, with no
    # second reboot to store it.
    assert result.reboots == 1, "panel rebooted %d times, not once%s" % (result.reboots, info)
    assert result.registration is not None, "panel did not register after the update%s" % info
    assert result.registration["friendly_name"] == rig.hostname, (
        "panel registered as %r, not %r: its settings were lost%s" % (
            result.registration["friendly_name"], rig.hostname, info))
    assert result.registration["md5_firmware"] == rig.update_server.checksum("firmware"), info
    assert result.registration["md5_data_file"] == "embedded", info
    assert result.mounted, "LittleFS on flash does not mount%s" % info
    assert result.config is not None, "config.json is missing from LittleFS%s" % info
    assert result.config.get("has_updated") is False, info
    assert result.config.get("md5_firmware") == rig.update_server.checksum("firmware"), info
