"""Run a firmware / LittleFS update on a Rig and judge the outcome.

The panel's settings (Wi-Fi, MQTT, name, stored checksums) live in
/littlefs/config.json, on the partition a LittleFS update replaces. So the
question every test here asks is: whatever went wrong with the download, did
the panel come back with its settings?

  L1  settings kept   after the update the panel boots, connects and registers
                      under its own name, and config.json still holds what it did
  L2  integrity       a stored LittleFS md5 matches the image on flash; an image
                      that did not arrive whole is never kept as the new one
  L3  liveness        the update ends (reboot, or back to idle) within the budget

The image the manager serves is a real LittleFS image, built from data/ like
PlatformIO's buildfs, with a marker in index.html so the test can tell the new
image from the one the panel booted with.
"""

import json
import os
import tempfile
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

from .image import build_littlefs, read_partitions
from .tft_server import Request

# A firmware md5 for tests that only update LittleFS: stored in the panel's
# config.json and served as /checksum_firmware, so the firmware is "up to date".
CURRENT_FIRMWARE_MD5 = "0123456789abcdef0123456789abcdef"

# The partition holding LittleFS (lib/LittleFSManager mounts the one labelled "spiffs").
FS_PARTITION = "spiffs"

# Budget for one update. A LittleFS download takes a few seconds; the worst
# correct case is 5 failed attempts with 2+4+8+16 s back-off, a format, and a
# reboot. A firmware update adds a 1.5 MB download and two reboots.
UPDATE_TIMEOUT = 420
# From a reboot to the first register_request (see rig.BOOT_TIMEOUT).
REGISTER_TIMEOUT = 180


@dataclass
class OtaResult:
    served_md5: str  # md5 of the LittleFS image the manager served
    rebooted: bool
    registration: Optional[dict]  # the first register_request after the update, if any
    files: Dict[str, bytes]  # what LittleFS holds after the update (read back from flash)
    mounted: bool  # whether the LittleFS partition on flash could be read at all
    downloads: List[Request]
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
        lines = ["rebooted=%s, registered as %r with md5_data_file=%r md5_firmware=%r, littlefs %s (%s), %.0fs%s" % (
            self.rebooted, reg.get("friendly_name"), reg.get("md5_data_file"), reg.get("md5_firmware"),
            "readable" if self.mounted else "UNREADABLE", ", ".join(sorted(self.files)) or "empty",
            self.elapsed, ", gave up: " + self.stalled if self.stalled else "")]
        for r in self.downloads:
            lines.append("  GET %s -> %s sent=%d/%d %s" % (r.path, r.status, r.sent, r.length, r.outcome))
        lines.append("  panel:")
        lines += ["    " + c for c in self.console[-40:]]
        return "\n".join(lines)


def partition(rig, name: str = FS_PARTITION) -> Dict[str, int]:
    return read_partitions(os.path.join(rig.panel.project_dir, "partitions.csv"))[name]


def make_littlefs(rig, marker: str, size: Optional[int] = None) -> bytes:
    """A LittleFS image as the manager stages it: data/, no config.json, `marker` in index.html."""
    data_dir = os.path.join(rig.panel.project_dir, "data")
    with open(os.path.join(data_dir, "index.html"), "rb") as f:
        index = f.read()
    files = {"index.html": index + ("\n<!-- %s -->\n" % marker).encode()}
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "littlefs.bin")
        build_littlefs(data_dir, files, size or partition(rig)["size"], out)
        with open(out, "rb") as f:
            return f.read()


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


def stage_littlefs_only(rig, marker: str = "littlefs-update") -> bytes:
    """Before boot: make the panel's firmware current, so a firmware update only updates LittleFS."""
    rig.panel.panel_config["md5_firmware"] = CURRENT_FIRMWARE_MD5
    rig.tft_server.override_checksum("firmware", CURRENT_FIRMWARE_MD5)
    image = make_littlefs(rig, marker)
    rig.tft_server.set_file("littlefs", image)
    return image


def start_firmware_update(rig) -> None:
    """Press "update firmware" in the manager, once the panel is listening for commands."""
    rig.broker.wait_for_subscription(rig.panel.client_id, "nspanel/%s/command" % rig.mac, timeout=60)
    rig.manager.send_command("firmware_update")


# A boot with no reboot for this long after it registered is the last one: the
# firmware update's second reboot comes about 1 s after registering.
SETTLED = 25


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


def run_update(rig, expect_reboot: bool = True, timeout: float = UPDATE_TIMEOUT,
               during: Optional[Callable[[], None]] = None, settle: float = 30) -> OtaResult:
    """Start a firmware update and wait for it to end.

    With `expect_reboot`, waits for the panel to reboot and register again; a
    firmware update reboots twice (once into the new image, once after storing
    its checksum), so it waits until a registration after the last reboot.
    Without it, waits `settle` seconds for a reboot that should not come.
    Never fails by itself; a hang comes back with `stalled` set.
    """
    served = rig.tft_server.checksum("littlefs")
    first_request = rig.tft_server.mark()
    console = rig.panel.mark()
    boots = rig.panel.boots
    t0 = time.monotonic()
    start_firmware_update(rig)
    if during is not None:
        during()

    stalled = None
    registration = None
    if expect_reboot:
        try:
            registration = _wait_until_settled(rig, boots, rig.manager.registration_mark(), t0 + timeout)
        except TimeoutError as e:
            stalled = str(e)
    else:
        end = time.monotonic() + settle
        while time.monotonic() < end and rig.panel.boots == boots:
            rig.panel.check_alive()
            time.sleep(0.5)
        if rig.panel.boots != boots:
            stalled = "panel rebooted"
    rebooted = rig.panel.boots > boots

    files = read_littlefs(rig) if rebooted else None
    return OtaResult(
        served_md5=served,
        rebooted=rebooted,
        registration=registration,
        files=files or {},
        mounted=files is not None,
        downloads=[r for r in rig.tft_server.requests[first_request:] if r.target == "littlefs"],
        console=[line for line in rig.panel.lines[console:]
                 if any(t in line for t in ("UpdateManager", "LittleFS", "ConfigManager", "Main:"))],
        elapsed=time.monotonic() - t0,
        stalled=stalled,
    )


def check_settings_kept(rig, result: OtaResult) -> None:
    """L1 and L3: the panel came back, registered under its own name, with its settings on flash."""
    info = "\n" + result.summary()
    assert result.stalled is None, "update did not finish: %s%s" % (result.stalled, info)
    assert result.rebooted, "panel did not reboot%s" % info
    assert result.registration is not None, "panel did not register after the update%s" % info
    assert result.registration["friendly_name"] == rig.hostname, (
        "panel registered as %r, not %r: its settings were lost%s" % (
            result.registration["friendly_name"], rig.hostname, info))
    assert result.mounted, "LittleFS on flash does not mount%s" % info
    config = result.config
    assert config is not None, "config.json is missing from LittleFS%s" % info
    for key in ("wifi_hostname", "wifi_ssid", "wifi_psk", "mqtt_server", "mqtt_port"):
        assert config.get(key) == rig.panel.panel_config[key], "config.json %s=%r, was %r%s" % (
            key, config.get(key), rig.panel.panel_config[key], info)


def check_new_image(rig, result: OtaResult, marker: str) -> None:
    """L2 on success: the served image is in place and its md5 stored."""
    info = "\n" + result.summary()
    index = result.files.get("index.html", b"")
    assert marker.encode() in index, "LittleFS does not hold the new image (no %r in index.html)%s" % (marker, info)
    assert result.registration["md5_data_file"] == result.served_md5, info


def check_fallback(rig, result: OtaResult) -> None:
    """L2 when every attempt failed: settings on an empty LittleFS, and its md5 cleared so it is offered again."""
    info = "\n" + result.summary()
    assert "index.html" not in result.files, "a failed image was kept as LittleFS%s" % info
    assert result.registration["md5_data_file"] == "", (
        "stored LittleFS md5 %r after a failed update%s" % (result.registration["md5_data_file"], info))
