"""GUI updates under faults: the network, the manager and the display misbehaving mid-update.

Every test checks the same invariants (nspm_qemu.gui_update): the firmware may
retry, give up or reboot, but it must never declare success, or store an md5,
for an image the display did not receive intact.
"""

import hashlib

import pytest

from nspm_qemu.gui_update import check_invariants, run_gui_update, start_gui_update
from test_gui_update import TFT_SIZE, make_tft
from nspm_qemu.fake_nextion import TERMINAL

pytestmark = pytest.mark.skipif(TERMINAL is not None, reason="NSPM's TFT only (NSPM_QEMU_TERMINAL=nextion)")

CHUNK = 4096
# A Django DEBUG=True error page, as the real manager serves on an exception.
ERROR_PAGE = b"<!DOCTYPE html><html><head><title>Server Error (500)</title></head><body>" + b"x" * 60000 + b"</body></html>"


@pytest.fixture
def staged(rig):
    """A booted panel with a TFT staged on the manager."""
    rig.boot()
    rig.set_tft(make_tft(45))
    return rig


# ---- the manager's HTTP side -----------------------------------------------

def test_survives_http_reset_mid_download(staged):
    """PR #45: a read error mid-download must resume, not flash the previous chunk again."""
    staged.update_server.reset_at(int(TFT_SIZE * 0.75))
    result = run_gui_update(staged)
    check_invariants(result)
    ranged = [d for d in result.downloads if d.range is not None]
    assert ranged[0].outcome == "reset" and len(ranged) >= 2, "the fault did not hit the download\n" + result.summary()


def test_survives_short_body(staged):
    """The manager closes cleanly before Content-Length: a read returns 0."""
    staged.update_server.fin_at(int(TFT_SIZE * 0.4))
    check_invariants(run_gui_update(staged))


def test_survives_http_stall(staged):
    """The stream stops for longer than the HTTP client's timeout."""
    staged.update_server.stall_at(int(TFT_SIZE * 0.5), 30)
    check_invariants(run_gui_update(staged))


def test_partial_tft_reads(staged):
    """Data arrives in small segments, so each 4096-byte chunk takes many reads."""
    staged.update_server.trickle(512, 0.002)
    check_invariants(run_gui_update(staged))


def after_reset(server, arm):
    """A `during` callback: once the armed reset has cut the download, run `arm` (before the panel reconnects)."""
    def during():
        # The panel waits 5 s after a failed read before reconnecting.
        server.wait_for_request(lambda r: r.outcome == "reset", what="reset download")
        arm()
    return during


def test_manager_down_then_back(staged):
    """The download drops and the next few reconnects are refused."""
    server = staged.update_server
    server.reset_at(int(TFT_SIZE * 0.3))
    check_invariants(run_gui_update(staged, during=after_reset(server, lambda: server.drop_next(4))))


def test_size_probe_fails(staged):
    """H7: the manager is unreachable for the panel's first request, which only reads the file size."""
    staged.update_server.drop_next(1)
    result = run_gui_update(staged)
    check_invariants(result, expect_success=False)
    sizes = {u.size for u in result.uploads}
    assert sizes <= {TFT_SIZE}, "panel announced upload size(s) %s for a %d-byte file\n%s" % (sizes, TFT_SIZE, result.summary())
    assert result.rebooted, "update never finished\n" + result.summary()


def test_resume_error_page_is_not_flashed(staged):
    """H1: a resume that gets a 500 error page must not write the page into the TFT."""
    server = staged.update_server
    server.reset_at(int(TFT_SIZE * 0.5))
    check_invariants(run_gui_update(staged, during=after_reset(server, lambda: server.respond_next(1, 500, ERROR_PAGE))))


def test_error_page_on_size_probe(staged):
    """H1: the manager answers the size probe with a 500 page; its length is not the TFT's size."""
    staged.update_server.respond_next(1, 500, ERROR_PAGE)
    result = run_gui_update(staged)
    check_invariants(result, expect_success=False)
    sizes = {u.size for u in result.uploads}
    assert sizes <= {TFT_SIZE}, "panel announced upload size(s) %s for a %d-byte file\n%s" % (sizes, TFT_SIZE, result.summary())


def test_ignored_range_is_not_flashed(staged):
    """H2: a resume answered with the whole file (Range ignored) must not be written at the resume offset."""
    server = staged.update_server
    server.reset_at(int(TFT_SIZE * 0.5))
    check_invariants(run_gui_update(staged, during=after_reset(server, lambda: server.ignore_range_next(1))))


def test_checksum_error_page_is_not_stored(staged):
    """The manager answers the md5 request with an error page once; the page must not be stored as the md5."""
    staged.update_server.checksum_respond_next(1, 500, ERROR_PAGE)
    check_invariants(run_gui_update(staged))


def test_missing_content_length(staged):
    """H7: the size probe gets no Content-Length; the panel must not start an upload of a bogus size."""
    staged.update_server.no_content_length_next(1)
    result = run_gui_update(staged)
    check_invariants(result, expect_success=False)
    sizes = {u.size for u in result.uploads}
    assert sizes <= {TFT_SIZE}, "panel announced upload size(s) %s for a %d-byte file\n%s" % (sizes, TFT_SIZE, result.summary())


@pytest.mark.xfail(strict=True, reason="H6: the md5 is fetched after the upload, not before it")
def test_tft_replaced_mid_update(staged):
    """H6: a new TFT staged mid-update; the md5 stored must be that of the image the display got."""
    new = make_tft(46)
    staged.nextion.valid_md5s.add(hashlib.md5(new).hexdigest())
    staged.update_server.swap_file_at(int(TFT_SIZE * 0.5), new)
    check_invariants(run_gui_update(staged), expect_success=False)


@pytest.mark.xfail(strict=True, reason="H6: nothing notices the file changed, so a resume mixes two files and an md5 is stored")
def test_tft_replaced_then_resumed(staged):
    """H6: a new TFT staged mid-update, then the download drops: the resume reads the new file.

    The display now holds the start of one file and the rest of another. No md5
    describes that image, so the firmware must not store one (and should start
    over or give up).
    """
    new = make_tft(46)
    staged.nextion.valid_md5s.add(hashlib.md5(new).hexdigest())
    staged.update_server.swap_file_at(int(TFT_SIZE * 0.5), new)
    staged.update_server.reset_at(int(TFT_SIZE * 0.6))
    result = run_gui_update(staged)
    check_invariants(result, expect_success=False)
    assert result.image_md5 in result.valid_md5s or not result.completed, result.summary()


# ---- retries that run out ----------------------------------------------------
# A correct panel gives up instead of retrying forever. Before the display is in
# upload mode it just carries on; after that it reboots, keeping the old GUI md5.

ABANDONED = r"Giving up on GUI update"
# Twenty download attempts two seconds apart, plus connect time: longer than the
# usual no-progress cutoff, so these tests allow more.
OUTAGE_IDLE_TIMEOUT = 150


def test_manager_unreachable_before_update(staged):
    """The size probe never succeeds: the panel gives up without touching the display, and a later update works."""
    server = staged.tft_server
    server.drop_next(1000)
    boots = staged.panel.boots
    mark = staged.panel.mark()
    start_gui_update(staged)
    staged.panel.wait_for_log(ABANDONED, timeout=180, since=mark)
    assert staged.nextion.uploads == [], "the display was put into upload mode"
    assert staged.panel.boots == boots, "the panel rebooted although the display was never touched"

    server.clear_faults()
    check_invariants(run_gui_update(staged))


def test_manager_goes_down_mid_update(staged):
    """The download drops and the manager never comes back: reboot, old GUI md5 kept."""
    server = staged.tft_server
    server.reset_at(int(TFT_SIZE * 0.5))
    result = run_gui_update(staged, idle_timeout=OUTAGE_IDLE_TIMEOUT, timeout=300,
                            during=after_reset(server, lambda: server.drop_next(1000)))
    check_invariants(result, expect_success=False)
    assert result.rebooted and not result.completed, result.summary()
    assert result.stored_md5 != server.md5, result.summary()


def test_jump_past_end_of_file(staged):
    """The display asks to continue from beyond the end of the file: give up, do not loop."""
    staged.nextion.jump_offset = TFT_SIZE + 8 * CHUNK
    result = run_gui_update(staged)
    check_invariants(result, expect_success=False)
    assert result.rebooted and not result.completed, result.summary()
    assert result.stored_md5 != staged.tft_server.md5, result.summary()


def test_checksum_unreachable_after_upload(staged):
    """The upload completes but the md5 never arrives: reboot without storing one (the manager will offer the update again)."""
    server = staged.tft_server
    server.checksum_respond_next(1000, 500, ERROR_PAGE)
    result = run_gui_update(staged, idle_timeout=OUTAGE_IDLE_TIMEOUT, timeout=300)
    check_invariants(result, expect_success=False)
    assert result.rebooted, result.summary()
    assert result.image == result.data, "the upload itself should have been fine\n" + result.summary()
    assert result.stored_md5 != server.md5, result.summary()


# ---- the display -----------------------------------------------------------

def test_display_ack_timeout(staged):
    """H3: one lost ack forces the 20 s re-init; the retry must start the display from byte 0."""
    staged.nextion.drop_ack_at = {10}
    result = run_gui_update(staged)
    check_invariants(result)
    assert len(result.uploads) == 2, result.summary()


def test_display_keeps_stalling(staged):
    """The display stalls at the same chunk on every attempt: after three timeouts the panel must give up and reboot."""
    staged.nextion.always_drop_ack_at = {10}
    result = run_gui_update(staged)
    check_invariants(result, expect_success=False)
    assert result.rebooted and not result.completed, result.summary()
    assert len(result.uploads) <= 3, "panel kept retrying past its three-strikes limit\n" + result.summary()


def test_display_rejects_upload(staged):
    """The display never acks whmi-wris: three timeouts, reboot, old md5 kept."""
    staged.nextion.reject_upload = True
    result = run_gui_update(staged)
    check_invariants(result, expect_success=False)
    assert result.rebooted and not result.completed, result.summary()
    assert result.stored_md5 != staged.update_server.md5, result.summary()


def test_jump_offset(staged):
    """H4: the display already holds the start of the file and jumps ahead (v1.2)."""
    staged.nextion.flash = staged.update_server.data
    staged.nextion.jump_offset = 20 * CHUNK
    result = run_gui_update(staged)
    check_invariants(result)
    assert result.uploads[-1].chunks[1][0] == 20 * CHUNK, result.summary()


@pytest.mark.xfail(strict=True, reason="H5: the UPDATING RX path expects 0x08 and its offset in separate UART reads")
def test_jump_reply_in_one_read(staged):
    """H5: the 0x08 and its offset arrive together, as one UART read."""
    staged.nextion.split_jump = False
    result = run_gui_update(staged)
    check_invariants(result)
    assert len(result.uploads) == 1, "the upload had to restart\n" + result.summary()


def test_duplicate_ack(staged):
    """H8: the display acks one chunk twice."""
    staged.nextion.dup_ack_at = {10}
    check_invariants(run_gui_update(staged), expect_success=False)


@pytest.mark.xfail(strict=True, reason="H8: the UPDATING RX path treats each UART read as one message")
def test_frames_sharing_a_read_with_an_ack(staged):
    """H8: another frame reaches the panel in the same UART read as an ack (here, a touch just ahead of it).

    The UPDATING receive path treats each UART read as one message and looks only
    at its first byte, so the ack behind the touch is lost.
    """
    staged.nextion.inject_at = {10: bytes([0x65, 0x00, 0x01, 0x01, 0xFF, 0xFF, 0xFF])}
    result = run_gui_update(staged)
    check_invariants(result)
    # Restarting after the 20 s ack timeout recovers, but only because the ack was lost; it must not be.
    assert len(result.uploads) == 1, "the upload had to restart\n" + result.summary()


# ---- the rest of the system ------------------------------------------------

def test_mqtt_drop_during_update(staged):
    """MQTT reconnects mid-update must not disturb the HTTP download or the UART."""
    staged.nextion.ack_delay = 0.05  # slow enough to land the drops mid-upload

    def drop_mqtt():
        staged.nextion.wait_for_upload_progress(10 * CHUNK)
        for _ in range(3):
            mark = staged.broker.event_mark()
            staged.broker.drop_client(staged.panel.client_id)
            staged.broker.wait_for_event("connect", staged.panel.client_id, timeout=60, since=mark)

    check_invariants(run_gui_update(staged, during=drop_mqtt))
