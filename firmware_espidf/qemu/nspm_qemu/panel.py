"""Run the firmware in Espressif's QEMU, wired to the fake display and broker.

    UART0  -> console, captured line by line (and to <workdir>/console.log)
    UART2  -> FakeNextion, over a TCP socket QEMU connects out to
    open_eth (user-mode net) -> host as 10.0.2.2, so the broker on 127.0.0.1
             is reachable from the panel; the panel's port 80 is forwarded
             to `web_port` on the host
"""

import logging
import os
import re
import shutil
import socket
import subprocess
import threading
import time
from typing import Callable, List, Optional

from .fake_nextion import FakeNextion
from .image import build_efuse_image, build_flash_image

log = logging.getLogger("panel")

# Lines that mean the firmware has crashed. PANIC_PRINT_HALT keeps it stopped
# there, so these are terminal.
CRASH_PATTERNS = [
    re.compile(r"Guru Meditation Error"),
    re.compile(r"abort\(\) was called"),
    re.compile(r"assert failed"),
    re.compile(r"ESP_ERROR_CHECK failed"),
    re.compile(r"\*\*\*ERROR\*\*\* A stack overflow"),
]

# Logged by Nextion::init() just before it installs the UART driver and
# power-cycles the display. The TFT announces itself a moment later.
NEXTION_POWER_ON = re.compile(r"Nextion: Initializing Nextion display")
NEXTION_BOOT_DELAY = 0.5

_ANSI = re.compile(r"\x1b\[[0-9;]*m")


def find_qemu() -> str:
    candidates = [
        os.environ.get("QEMU_XTENSA"),
        shutil.which("qemu-system-xtensa"),
        os.path.expanduser("~/.cache/nspm-qemu/qemu/bin/qemu-system-xtensa"),
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise FileNotFoundError("qemu-system-xtensa not found; run qemu/install_qemu.sh or set QEMU_XTENSA")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class PanelCrashed(AssertionError):
    pass


class QemuPanel:
    def __init__(self, workdir: str, build_dir: str, project_dir: str, mac: str, panel_config: Optional[dict]):
        self.workdir = workdir
        self.build_dir = build_dir
        self.project_dir = project_dir
        self.mac = mac
        self.panel_config = panel_config
        self.nextion = FakeNextion()
        self.web_port = _free_port()
        self.lines: List[str] = []
        self.crash: Optional[str] = None
        self._cond = threading.Condition()
        self._proc: Optional[subprocess.Popen] = None
        self._console_log = None
        self._reader: Optional[threading.Thread] = None
        self.boot_display = True  # emit the TFT's NSPM flag on power-on

    @property
    def client_id(self) -> str:
        return "NSPMPanel-" + self.mac

    # ---- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        os.makedirs(self.workdir, exist_ok=True)
        flash = os.path.join(self.workdir, "flash.bin")
        efuse = os.path.join(self.workdir, "efuse.bin")
        build_flash_image(self.build_dir, self.project_dir, self.panel_config, flash)
        build_efuse_image(self.mac, efuse)

        args = [
            find_qemu(),
            "-M", "esp32",
            "-m", "4M",  # PSRAM; CONFIG_SPIRAM_BOOT_INIT fails without it
            "-display", "none",
            "-monitor", "none",
            "-serial", "stdio",
            "-serial", "null",
            "-serial", "tcp:127.0.0.1:%d" % self.nextion.port,
            "-drive", "file=%s,if=mtd,format=raw" % flash,
            "-drive", "file=%s,if=none,format=raw,id=efuse" % efuse,
            "-global", "driver=nvram.esp32.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth,hostfwd=tcp:127.0.0.1:%d-:80" % self.web_port,
        ]
        # NSPM_QEMU_GDB=1 exposes a gdbstub on :3333; =wait also halts at reset until gdb attaches.
        gdb = os.environ.get("NSPM_QEMU_GDB")
        if gdb:
            args += ["-gdb", "tcp::3333"]
            if gdb == "wait":
                args += ["-S"]
                log.warning("waiting for gdb on :3333")
        log.info("starting %s", " ".join(args))
        self._console_log = open(os.path.join(self.workdir, "console.log"), "w")
        # stdin must stay open: EOF on QEMU's stdio chardev stalls UART0, and
        # with it every task that logs.
        self._proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self._reader = threading.Thread(target=self._read_console, name="qemu-console", daemon=True)
        self._reader.start()

    def stop(self) -> None:
        if self._proc is not None and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        if self._reader is not None:
            self._reader.join(5)
        self.nextion.close()
        if self._console_log is not None:
            self._console_log.close()
            self._console_log = None

    @property
    def running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    # ---- console -----------------------------------------------------------

    def mark(self) -> int:
        with self._cond:
            return len(self.lines)

    def wait_for_log(self, pattern: str, timeout: float = 60, since: int = 0) -> re.Match:
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        with self._cond:
            pos = since
            while True:
                for i in range(pos, len(self.lines)):
                    m = rx.search(self.lines[i])
                    if m:
                        return m
                pos = len(self.lines)
                self.check_alive()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("console: no line matching %r within %.0fs" % (pattern, timeout))
                self._cond.wait(min(remaining, 1.0))

    def log_lines(self, pattern: str, since: int = 0) -> List[str]:
        rx = re.compile(pattern)
        with self._cond:
            return [line for line in self.lines[since:] if rx.search(line)]

    def check_alive(self) -> None:
        if self.crash is not None:
            raise PanelCrashed("firmware crashed: %s\n%s" % (self.crash, self.tail(40)))
        if self._proc is not None and self._proc.poll() is not None:
            raise PanelCrashed("QEMU exited with %s\n%s" % (self._proc.returncode, self.tail(40)))

    def tail(self, n: int = 40) -> str:
        with self._cond:
            return "\n".join(self.lines[-n:])

    def _read_console(self) -> None:
        assert self._proc is not None and self._proc.stdout is not None
        for raw in self._proc.stdout:
            line = _ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip("\r\n")
            if self._console_log is not None:
                self._console_log.write(line + "\n")
                self._console_log.flush()
            with self._cond:
                self.lines.append(line)
                if self.crash is None and any(p.search(line) for p in CRASH_PATTERNS):
                    self.crash = line
                self._cond.notify_all()
            if self.boot_display and NEXTION_POWER_ON.search(line):
                threading.Timer(NEXTION_BOOT_DELAY, self._display_boot).start()
        with self._cond:
            self._cond.notify_all()

    def _display_boot(self) -> None:
        try:
            self.nextion.boot()
        except (OSError, RuntimeError) as e:
            log.warning("could not send NSPM flag: %s", e)

    # ---- helpers -----------------------------------------------------------

    def wait_until(self, predicate: Callable[[], bool], timeout: float = 30, what: str = "condition") -> None:
        deadline = time.monotonic() + timeout
        while not predicate():
            self.check_alive()
            if time.monotonic() > deadline:
                raise TimeoutError("%s not met within %.0fs" % (what, timeout))
            time.sleep(0.1)
