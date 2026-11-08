"""Fake device: the Nextion display on the emulated ESP32's UART2. It is nextion-tools' emulated
Display running NSPanelManager's TFT, plus the calls tests use to touch it and read it back.

nextion_tools.emu models the display and runs the TFT's own code from gui.HMI (see
https://github.com/jamesmulcahy/nextion-tools; requirements.txt pins it).
So the "NSPM" banner, the 0x92 sleep frame and the 0x93 wake frame all come from the HMI's code,
as on a real panel: the banner from page0's load, sleep from each page's sleep_display timer, and
wake from the screensaver's touch capture. What is left here is how a test asks for them.
"""

import logging
import os
import re
from typing import Optional, Union

from nextion_tools.emu import Display, TcpServer

log = logging.getLogger("fake_nextion")

# Frames the NSPanel Manager TFT's code sends (lib/Nextion/Nextion_constans.hpp).
SLEEP = 0x92
WAKE = 0x93
STRING_DATA = 0x99

SCREENSAVER_PAGES = ("screensaver", "screensaver2")


class FakeNextion(Display):
    def __init__(self, **kwargs):
        if kwargs.get("hmi") is None or not kwargs.get("scripts", True):
            # Without the HMI's code nothing else prints it.
            kwargs.setdefault("banner", b"NSPM\xff\xff\xff")
        super().__init__(TcpServer(), **kwargs)
        # Where screenshot() saves, and where every page wait_for_page() sees is saved. None
        # saves nothing.
        self.screenshots_dir: Optional[str] = None
        self._screenshots = 0

    def screenshot(self, name: str) -> Optional[str]:
        """Save what the screen shows as <screenshots_dir>/NN-<name>.png. Never fails a test:
        returns None, and logs why, if it could not."""
        if self.screenshots_dir is None or self.screen is None:
            return None
        self._screenshots += 1
        path = os.path.join(self.screenshots_dir, "%02d-%s.png" % (self._screenshots, re.sub(r"[^\w.-]", "_", name)))
        try:
            os.makedirs(self.screenshots_dir, exist_ok=True)
            return self.save_png(path)
        except Exception as e:
            log.warning("could not save %s: %s", path, e)
            return None

    def wait_for_page(self, page: str, timeout: float = 30) -> None:
        super().wait_for_page(page, timeout)
        self.screenshot(page)

    @property
    def port(self) -> int:
        """Where QEMU's `-serial tcp:` connects."""
        return self.transport.port

    def sleep(self) -> None:
        """The screensaver timeout runs out on the page shown."""
        if not self.runs_scripts:
            self.send_frame(SLEEP)
            return
        self.expire_timer("sleep_display")

    def wake(self, wait: float = 10) -> bool:
        """Someone taps the screensaver, once one is shown (waiting up to `wait` seconds: the
        firmware logs that it shows the screensaver before its `page` command reaches the
        display). Returns False, having touched nothing, if none is shown by then, since a tap
        elsewhere would land on a real button."""
        if not self.runs_scripts:
            self.send_frame(WAKE)
            return True
        with self._cond:
            self._cond.wait_for(lambda: self.page in SCREENSAVER_PAGES, wait)
            if self.page not in SCREENSAVER_PAGES:
                return False
            w, h = self._current_page().size
        self.tap(w // 2, h // 2)
        return True

    def send_string(self, data: Union[bytes, str]) -> None:
        """A 0x99 string event (the TFT's printh-style custom data)."""
        if isinstance(data, str):
            data = data.encode()
        self.send_frame(STRING_DATA, data)
