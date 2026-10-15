"""Page switches that race each other across event loops."""

import time

BUTTON_CEILING = 22


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
    rig.manager.wait_for_registration(timeout=180)
    rig.panel.wait_for_log(r"Showing screensaver page", timeout=60)

    # The real display reports one wake per touch of the screensaver.
    for _ in range(10):
        rig.nextion.wake()
        time.sleep(0.1)

    rig.nextion.wait_for_page("home")
    time.sleep(1)  # let the last switches settle

    # A frozen panel still accepts UART input but never handles it.
    mark = rig.manager.command_mark()
    rig.nextion.press(BUTTON_CEILING)
    rig.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark, timeout=15)
