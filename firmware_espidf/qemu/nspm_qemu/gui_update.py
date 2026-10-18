"""Run a GUI update on a Rig and judge the outcome.

A GUI update is only correct if the display ends up holding the exact file the
manager served. The firmware's own view ("Nextion GUI update complete", the md5
it stores) is what decides whether it ever tries again, so these helpers check
the two against each other:

  I1  integrity      a stored md5 matches the display's image, byte for byte
  I2  no false win   "update complete" is only logged for a correct image
  I3  liveness       the update ends (reboot) within the test's budget
  I4  protocol       full 4096-byte chunks except the last, nothing past the end
"""

import hashlib
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional

from .fake_nextion import UPDATE_CHUNK, Upload
from .tft_server import Request

COMPLETE = r"Nextion GUI update complete"


@dataclass
class UpdateResult:
    data: bytes  # the file the manager served when the update started
    uploads: List[Upload]  # every upload the display saw during this update
    completed: bool  # the firmware logged COMPLETE
    rebooted: bool
    stored_md5: Optional[str]  # md5_tft_gui after the reboot, if it rebooted
    display_state: str
    downloads: List[Request]
    elapsed: float
    valid_md5s: set = field(default_factory=set)
    stalled: Optional[str] = None  # why run_gui_update stopped waiting, if the panel never rebooted

    @property
    def image(self) -> Optional[bytes]:
        """What the display holds now: the last complete upload's image."""
        done = [u for u in self.uploads if u.complete]
        return bytes(done[-1].image) if done else None

    @property
    def image_md5(self) -> Optional[str]:
        return hashlib.md5(self.image).hexdigest() if self.image is not None else None

    def summary(self) -> str:
        lines = ["%d upload(s), completed=%s, rebooted=%s, stored_md5=%s, display=%s, %.0fs%s" % (
            len(self.uploads), self.completed, self.rebooted, self.stored_md5, self.display_state, self.elapsed,
            ", gave up: " + self.stalled if self.stalled else "")]
        for i, u in enumerate(self.uploads, 1):
            lines.append("  upload %d: %s %d/%d bytes in %d chunks%s%s" % (
                i, u.protocol, u.cursor, u.size, len(u.chunks),
                " complete" if u.complete else "", " ABANDONED" if u.abandoned else ""))
        # Retry loops repeat the same request hundreds of times; show each run once with a count.
        previous, repeats = None, 0
        for r in self.downloads + [None]:
            line = None if r is None else "  GET %s range=%s -> %s start=%d sent=%d/%d %s" % (
                r.path, r.range, r.status, r.start, r.sent, r.length, r.outcome)
            if line == previous:
                repeats += 1
                continue
            if repeats:
                lines.append("    ... repeated %d more times" % repeats)
            if line is not None:
                lines.append(line)
            previous, repeats = line, 0
        return "\n".join(lines)


def start_gui_update(rig) -> None:
    """Press "update GUI" in the manager, once the panel is listening for commands."""
    # boot() returns at the screensaver, which is shown before the panel has
    # subscribed to its MAC command topic; a command sent earlier is lost.
    rig.broker.wait_for_subscription(rig.panel.client_id, "nspanel/%s/command" % rig.mac, timeout=60)
    rig.manager.send_command("tft_update")


# A correct update takes 10-30 s (guest time); the slowest correct fault case,
# a display that never acks, takes ~90 s (3 x (20 s ack timeout + 10 s restart)).
UPDATE_TIMEOUT = 180
# The longest the display legitimately sees nothing (no bytes, no new upload):
# a 20 s ack timeout plus the 10 s display restart. Twice that means a hang.
IDLE_TIMEOUT = 60


def run_gui_update(rig, timeout: float = UPDATE_TIMEOUT, idle_timeout: float = IDLE_TIMEOUT,
                   during: Optional[Callable[[], None]] = None) -> UpdateResult:
    """Start a GUI update and wait until the firmware reboots.

    Gives up after `timeout`, or sooner once the display has made no progress
    for `idle_timeout`. `during` runs right after the update is requested, to
    inject faults that need the update under way. Never fails by itself: a
    hang comes back as rebooted=False (with `stalled` saying why), for the
    test to judge.
    """
    data = rig.tft_server.data
    first_upload = len(rig.nextion.uploads)
    first_request = rig.tft_server.mark()
    console = rig.panel.mark()
    boots = rig.panel.boots
    t0 = time.monotonic()
    start_gui_update(rig)
    if during is not None:
        during()

    deadline = t0 + timeout
    progress, last_progress = None, time.monotonic()
    stalled = None
    while rig.panel.boots == boots:
        rig.panel.check_alive()
        now = time.monotonic()
        uploads = rig.nextion.uploads[first_upload:]
        current = (len(uploads), uploads[-1].cursor if uploads else 0)
        if current != progress:
            progress, last_progress = current, now
        elif now - last_progress > idle_timeout:
            stalled = "display made no progress for %.0fs" % (now - last_progress)
            break
        if now > deadline:
            stalled = "no reboot within %.0fs" % timeout
            break
        time.sleep(0.5)
    rebooted = rig.panel.boots > boots
    completed = bool(rig.panel.log_lines(COMPLETE, since=console))

    stored = None
    if rebooted:
        mark = rig.broker.mark()
        stored = rig.status_report(since=mark, timeout=180).md5_tft_gui
    return UpdateResult(
        data=data,
        uploads=list(rig.nextion.uploads[first_upload:]),
        completed=completed,
        rebooted=rebooted,
        stored_md5=stored,
        display_state=rig.nextion.display_state,
        downloads=[r for r in rig.tft_server.requests[first_request:] if r.path.startswith("/download_tft/")],
        elapsed=time.monotonic() - t0,
        valid_md5s=set(rig.nextion.valid_md5s),
        stalled=stalled,
    )


def first_difference(a: bytes, b: bytes) -> Optional[int]:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return None if len(a) == len(b) else min(len(a), len(b))


def describe_mismatch(image: bytes, data: bytes) -> str:
    """Where the image goes wrong, and where the wrong bytes came from in the file (if anywhere)."""
    at = first_difference(image, data)
    if at is None:
        return "images match"
    chunk = at // UPDATE_CHUNK
    wrong = image[chunk * UPDATE_CHUNK:(chunk + 1) * UPDATE_CHUNK]
    source = data.find(wrong[:64]) if len(wrong) >= 64 else -1
    whence = ("bytes at that chunk are the file's bytes from offset %d (stale or misplaced chunk)" % source
              if source >= 0 else "bytes at that chunk do not come from the file")
    return "image differs from the served file at byte %d (chunk %d of %d); %s" % (
        at, chunk + 1, (len(data) + UPDATE_CHUNK - 1) // UPDATE_CHUNK, whence)


def check_invariants(result: UpdateResult, expect_success: bool = True) -> None:
    """Assert I1, I2, I4, and with `expect_success` also I3 and that the new GUI is in place."""
    info = "\n" + result.summary()

    # I4: each upload's chunks are full size except an upload's final one, and nothing overran.
    for i, u in enumerate(result.uploads, 1):
        assert u.overrun == 0, "upload %d: panel sent %d bytes past the image%s" % (i, u.overrun, info)
        sizes = [length for _, length, _ in u.chunks]
        short = [n for n, s in enumerate(sizes[:-1], 1) if s != UPDATE_CHUNK]
        assert not short, "upload %d: chunk(s) %s were not %d bytes%s" % (i, short, UPDATE_CHUNK, info)

    image = result.image
    # I1: whatever md5 the firmware stored must describe the display's actual image.
    if result.stored_md5 and result.completed:
        assert image is not None, "firmware stored md5 %s but no upload completed%s" % (result.stored_md5, info)
        assert result.stored_md5 == result.image_md5, (
            "firmware stored md5 %s but the display holds %s: %s%s" % (
                result.stored_md5, result.image_md5, describe_mismatch(image, result.data), info))

    # I2: "update complete" only for an image the manager actually served.
    if result.completed:
        assert image is not None, "firmware logged %r but the display never received a whole image%s" % (COMPLETE, info)
        assert result.image_md5 in result.valid_md5s, (
            "firmware logged %r for a corrupt image: %s%s" % (COMPLETE, describe_mismatch(image, result.data), info))

    if expect_success:
        assert result.rebooted, "update did not finish (I3): %s%s" % (result.stalled, info)
        assert result.completed, "panel rebooted without completing the update%s" % info
        assert image is not None, "no upload completed%s" % info
        assert image == result.data, describe_mismatch(image, result.data) + info
        assert result.stored_md5 == hashlib.md5(result.data).hexdigest(), info
        assert result.display_state == "ok", info
