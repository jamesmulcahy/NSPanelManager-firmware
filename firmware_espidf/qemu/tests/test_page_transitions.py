"""Page switches that race each other across event loops."""

import time

import pytest

from nspm_qemu import gui
from nspm_qemu.fake_nextion import WAKE
from nspm_qemu.proto import nspanel as pb
from nspm_qemu.rig import BOOT_TIMEOUT


def test_tapping_screensaver_during_first_load_does_not_deadlock(rig):
    """Tapping the screensaver while the panel finishes loading must not freeze it.

    Right after the screensaver first appears, RoomManager reports the home page
    status and the panel switches to the home page from the RoomManager loop. A
    tap on the screensaver switches pages from the default loop. Each switch
    registers handlers on the other loop, so if both run at once each loop task
    holds its own loop's mutex while waiting for the other's, and the UI freezes
    for good. Tap repeatedly across that window to hit it, at a pace a person
    tapping a blank screen could manage.
    """
    rig.start()
    rig.manager.wait_for_registration(timeout=BOOT_TIMEOUT)
    rig.panel.wait_for_log(r"Showing screensaver page", timeout=60)

    # Each tap on the screensaver makes its code print one wake. Once the home
    # page is up, the taps stop rather than land on its buttons.
    rig.nextion.wait_for(lambda n: n.page in ("screensaver", "screensaver2"), what="screensaver shown")
    for _ in range(10):
        rig.nextion.wake(wait=0)
        time.sleep(0.1)

    rig.nextion.wait_for_page("home")
    time.sleep(1)  # let the last switches settle

    # A frozen panel still accepts UART input but never handles it.
    mark = rig.manager.command_mark()
    rig.nextion.press(gui.HOME_CEILING)
    rig.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark, timeout=15)



@pytest.mark.parametrize("gap", [0.1, 1.1])
def test_waking_twice_to_entities_page_keeps_it_working(rig, gap):
    """Showing the entities page again before its last unshow has run must not leave it dead.

    With the entities page as the default page, every wake from the screensaver
    shows it, and the real display can report two wakes for what a person sees
    as one touch (a real panel did, 1.1 s apart). Each show() hands the
    previous page's unshow to another task. When that page is the entities page
    itself, a late unshow used to remove the handlers the newer show() had just
    registered and clear the page it was drawing, so the page stayed on screen
    with every button ignored until the screensaver came back.
    """
    rig.manager.config.default_page = pb.NSPanelConfig.ENTITIES
    rig.boot()

    # One tap, which the screensaver's code reports as a wake, then the display's
    # second report of it: by then the page may have changed, so no tap could make it.
    rig.nextion.wake()
    time.sleep(gap)
    rig.nextion.send_frame(WAKE)

    rig.nextion.wait_for_page("items4")
    rig.nextion.wait_for(lambda n: n.get("items4.i2_label.txt") == "   Fan", what="second entity slot drawn")
    time.sleep(1)  # let both unshows run

    mark = rig.manager.command_mark()
    rig.nextion.press(gui.ITEMS4_SLOT1)
    cmd = rig.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "toggle_entity_from_entities_page",
                                       since=mark, timeout=15)
    assert cmd.toggle_entity_from_entities_page.entity_page_id == rig.manager.rooms[1].entity_page_ids[0]
    assert cmd.toggle_entity_from_entities_page.entity_slot == 0
