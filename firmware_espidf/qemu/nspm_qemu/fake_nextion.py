"""Fake device: the Nextion display on the emulated ESP32's UART2. It is nextion-tools' emulated
Display running NSPanelManager's TFT, plus the calls tests use to touch it and read it back.

nextion_tools.emu models the display and runs the TFT's own code from gui.HMI (see
https://github.com/jamesmulcahy/nextion-tools; requirements.txt pins it).
So the "NSPM" banner, the 0x92 sleep frame and the 0x93 wake frame all come from the HMI's code,
as on a real panel: the banner from page0's load, sleep from each page's sleep_display timer, and
wake from the screensaver's touch capture. What is left here is how a test asks for them.

By default (NSPM_QEMU_TERMINAL unset, or us, or eu-on-us) the display runs nspanel-terminal's TFT: no
pages and no NSPM banner, a `connect` reply with native version 94, so the firmware's LvglUi
starts a NextionTerminal session and streams LVGL's pixels (nextion-tools' terminal receiver,
protocol 8). The screen is then the receiver's framebuffer, shown turned as the firmware turns it
for the panel (NextionTerminal::options_for()), and touches become terminal touch samples.
"""

import logging
import os
import re
import time
from typing import Optional, Union

from nextion_tools.emu import Display, TcpServer
from nextion_tools.emu import terminal as _terminal

log = logging.getLogger("fake_nextion")

# Frames the NSPanel Manager TFT's code sends (lib/Nextion/Nextion_constans.hpp).
SLEEP = 0x92
WAKE = 0x93
STRING_DATA = 0x99

SCREENSAVER_PAGES = ("screensaver", "screensaver2")

# The terminal receiver the display runs: "us" (320x480, which the firmware draws turned 180
# degrees; the default) or "eu-on-us" (454x320 on a US panel: turned 90, touch offset 26).
# NSPM_QEMU_TERMINAL=nextion runs NSPM's own Nextion TFT instead, and TERMINAL is then None.
_TERMINAL_ENV = os.environ.get("NSPM_QEMU_TERMINAL", "us")
if _TERMINAL_ENV not in ("us", "eu-on-us", "nextion"):
    raise ValueError("NSPM_QEMU_TERMINAL must be us, eu-on-us or nextion, not %r" % _TERMINAL_ENV)
TERMINAL = None if _TERMINAL_ENV == "nextion" else _TERMINAL_ENV
# How the firmware turns the screen onto each receiver (NextionTerminal::options_for()), so that
# snapshots and the viewer show the screen upright.
TERMINAL_ROTATION = {"us": 180, "eu-on-us": 90, "eu": 0}
# The receiver ends a session after 3 s without input, timed on the host's clock. Under QEMU the
# guest stops for ~20 s while it writes LittleFS with the flash cache off (see Rig.boot()), which a
# real panel does in milliseconds, so that watchdog would end every session at boot. The firmware
# keeps its own reverse timeout, on the guest's clock, so a sender that really goes quiet still
# fails there.
_terminal.WATCHDOG_S = 60.0


class FakeNextion(Display):
    def __init__(self, **kwargs):
        terminal = kwargs.get("terminal")
        if terminal is not None:
            # nspanel-terminal's TFT prints no banner: the firmware must not see NSPM's flag.
            kwargs.setdefault("rotation", TERMINAL_ROTATION.get(str(terminal), 0))
        elif kwargs.get("hmi") is None or not kwargs.get("scripts", True):
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
        if self.screenshots_dir is None or (self.screen is None and not self.showing_terminal):
            return None
        self._screenshots += 1
        path = os.path.join(self.screenshots_dir, "%02d-%s.png" % (self._screenshots, re.sub(r"[^\w.-]", "_", name)))
        try:
            os.makedirs(self.screenshots_dir, exist_ok=True)
            return self.save_png(path)
        except Exception as e:
            log.warning("could not save %s: %s", path, e)
            return None

    # Nextion::restart() keeps the display's power off for 250 ms, and what reaches it meanwhile is
    # lost on a panel. The emulator resets the moment the harness reads the restart's log line, which
    # can be in the middle of an instruction sent just before (a dim=, say): the rest would then be
    # read as an instruction of its own and rejected. So drop what arrives in the first 100 ms: enough
    # for bytes already in flight, and well inside the 250 ms even though the window starts when the
    # harness reads the line, a little after the firmware wrote it.
    POWER_OFF_S = 0.1

    def power_cycle(self) -> None:
        super().power_cycle()
        self._off_until = time.monotonic() + self.POWER_OFF_S

    def _on_data(self, data: bytes) -> None:
        if time.monotonic() < getattr(self, "_off_until", 0.0):
            log.debug("display off (power cycle): dropped %d bytes", len(data))
            return
        super()._on_data(data)

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
        if self.terminal_profile is not None:
            # LVGL's screensaver: a tap anywhere wakes it.
            self.wait_for_terminal(wait)
            w, h = self.size
            self.tap(w // 2, h // 2)
            return True
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

    def tap_screen(self, x: int, y: int, hold: float = 0.1) -> None:
        """A tap at (x, y) of the screen as shown (screenshots, the viewer): for the terminal, the
        firmware's own LVGL coordinates, cropped as the receiver crops them."""
        self.tap(*self.unrotate(x, y), hold=hold)

    def send_string(self, data: Union[bytes, str]) -> None:
        """A 0x99 string event (the TFT's printh-style custom data)."""
        if isinstance(data, str):
            data = data.encode()
        self.send_frame(STRING_DATA, data)
