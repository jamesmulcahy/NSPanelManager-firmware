"""Library: builds the flash image, then boots and runs the firmware in Espressif's QEMU, wired
to the fake display and broker, and captures its console.

    UART0  -> console, captured line by line (and to <workdir>/console.log)
    UART2  -> FakeNextion, over a TCP socket QEMU connects out to
    open_eth (user-mode net) -> host as 10.0.2.2, so the broker on 127.0.0.1
             is reachable from the panel; the panel's port 80 is forwarded
             to `web_port` on the host
"""

import itertools
import logging
import os
import re
import shutil
import socket
import subprocess
import threading
import time
from typing import Callable, Dict, List, Optional, Tuple

from .fake_nextion import TERMINAL, FakeNextion
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

# Logged by Nextion::restart(), which switches the display off and on: at every boot, when an
# operating baud rate falls back, when the link is recovered, when a GUI update starts, and when a
# terminal session ends (Nextion::terminal_release(true)). The
# fake display cannot see the GPIO, so this stands in for it: the display loses everything it
# held (an upload in progress included), and its TFT announces itself a moment later.
NEXTION_POWER_CYCLE = re.compile(r"Nextion: Power-cycling Nextion display")
NEXTION_BOOT_DELAY = 0.5

# The ROM's reset banner, printed on power-on and after every esp_restart().
ROM_RESET = re.compile(r"^rst:0x[0-9a-f]+ \((\w+)\)")

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


GDB_DIR = os.path.expanduser("~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin")


def _python_for_gdb(version: str) -> Optional[Tuple[str, str]]:
    """(PYTHONHOME, library dir) of a Python `version` with a shared libpython, from uv or PATH."""
    exes = []
    uv = shutil.which("uv")
    if uv:
        found = subprocess.run([uv, "python", "find", version], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if found.returncode == 0:
            exes.append(found.stdout.strip())
    if shutil.which("python" + version):
        exes.append(shutil.which("python" + version))
    for exe in exes:
        try:
            out = subprocess.run([exe, "-c", "import sys, sysconfig; print(sys.base_prefix); print(sysconfig.get_config_var('LIBDIR'))"],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=10).stdout.split("\n")
        except (OSError, subprocess.TimeoutExpired):
            continue
        if len(out) >= 2 and os.path.exists(os.path.join(out[1], "libpython%s.so" % version)):
            return out[0], out[1]
    return None


def find_gdb() -> Tuple[List[str], Dict[str, str]]:
    """gdb's command line and environment, with Python scripting if possible.

    Espressif ships one gdb per Python version (3.6 to 3.11), each needing that libpython. A
    newer system Python makes their wrapper fall back to the build without Python, which cannot
    run freertos_tasks.py, so look for a Python those builds can use (from uv or PATH) first.
    """
    env = dict(os.environ)
    if os.environ.get("XTENSA_GDB"):
        return [os.environ["XTENSA_GDB"]], env
    for version in ("3.11", "3.10", "3.9", "3.8"):
        binary = os.path.join(GDB_DIR, "xtensa-esp-elf-gdb-" + version)
        found = os.path.exists(binary) and _python_for_gdb(version)
        if found:
            env["PYTHONHOME"], env["LD_LIBRARY_PATH"] = found
            return [binary, "--mcpu=esp32"], env
    for c in (shutil.which("xtensa-esp32-elf-gdb"), os.path.join(GDB_DIR, "xtensa-esp32-elf-gdb")):
        if c and os.path.exists(c):
            return [c], env
    raise FileNotFoundError("xtensa-esp32-elf-gdb not found; set XTENSA_GDB")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def gdb_mode() -> Optional[str]:
    """NSPM_QEMU_GDB: None when unset, empty or "0" (off), else "1" or "wait"."""
    mode = os.environ.get("NSPM_QEMU_GDB", "")
    return None if mode in ("", "0") else mode


CLOCK_SAMPLE_GAP = 1.0  # seconds of host time between dump_cpus()'s two clock samples


def _clock_sample(gdb_output: bytes) -> Optional[List[int]]:
    """[CPU0 ccount, CPU1 ccount, xTickCount] from the first three `$N = value` lines, or None."""
    values = re.findall(rb"^\$\d+ = (0x[0-9a-fA-F]+|\d+)", gdb_output, re.M)[:3]
    return [int(v, 0) for v in values] if len(values) == 3 else None


def _clock_report(first: Optional[List[int]], second: Optional[List[int]]) -> str:
    """The header of cpus.txt: how far each CPU's clock and the tick moved between the samples."""
    if first is None or second is None:
        return "Clock check failed: could not read ccount/xTickCount twice.\n\n"
    names = ("CPU0 ccount", "CPU1 ccount", "xTickCount")
    moved = ", ".join("%s +%d" % (n, (b - a) & 0xFFFFFFFF) for n, a, b in zip(names, first, second))
    return ("Clock check, two samples %.1f s apart (host time): %s.\n"
            "A ccount that does not move: that CPU is stopped in the emulator. ccount moving but\n"
            "xTickCount not: the tick interrupt is not being taken.\n\n" % (CLOCK_SAMPLE_GAP, moved))


class PanelCrashed(AssertionError):
    pass


class QemuPanel:
    def __init__(self, workdir: str, build_dir: str, project_dir: str, mac: str, panel_config: Optional[dict],
                 display: Optional[str] = None):
        self.workdir = workdir
        self.build_dir = build_dir
        self.project_dir = project_dir
        self.mac = mac
        self.panel_config = panel_config
        # The TFT the display starts with: NSPM_QEMU_TERMINAL's, unless a test asks for "hmi" (NSPM's
        # own TFT), "terminal" (the US terminal TFT, or the one NSPM_QEMU_TERMINAL names), or a terminal
        # receiver by name ("us", "eu-on-us": the landscape layout on US hardware).
        if display not in (None, "hmi", "terminal", "us", "eu-on-us"):
            raise ValueError("display must be 'hmi', 'terminal', 'us' or 'eu-on-us', not %r" % display)
        if display == "hmi":
            self.terminal_kind: Optional[str] = None
        elif display == "terminal":
            self.terminal_kind = TERMINAL or "us"
        elif display is not None:
            self.terminal_kind = display
        else:
            self.terminal_kind = TERMINAL
        if self.terminal_kind:
            self.nextion = FakeNextion(terminal=self.terminal_kind)
            if self.terminal_kind != "us" and self.panel_config is not None:
                # LvglUi picks the receiver from config.json: the EU one unless lvgl_us_layout.
                self.panel_config = dict(self.panel_config, lvgl_us_layout=False)
        else:
            self.nextion = FakeNextion()
        self.web_port = _free_port()
        # A port of its own, so suites can run side by side with the stub on. `wait` is for a
        # person attaching by hand, so it keeps the well-known :3333.
        mode = gdb_mode()
        self.gdb_port: Optional[int] = None if mode is None else 3333 if mode == "wait" else _free_port()
        self.lines: List[str] = []
        self.crash: Optional[str] = None
        self.resets: List[str] = []  # reset reasons, one per boot (POWERON_RESET, SW_CPU_RESET, ...)
        self._cond = threading.Condition()
        self._proc: Optional[subprocess.Popen] = None
        self._console_log = None
        self._reader: Optional[threading.Thread] = None
        self.blank_littlefs = False  # flash a blank (all 0xFF) LittleFS partition instead of config.json
        self.raw_config_json: Optional[bytes] = None  # config.json exactly as given, in place of panel_config
        self.boot_display = True  # emit the TFT's NSPM flag on power-on
        self._stopping = False
        self._relaunching = False
        self._early_output = b""  # see _exited_on_a_port_clash()

    @property
    def client_id(self) -> str:
        return "NSPMPanel-" + self.mac

    # ---- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        os.makedirs(self.workdir, exist_ok=True)
        build_flash_image(self.build_dir, self.project_dir, self.panel_config, self._flash, self.blank_littlefs,
                          self.raw_config_json)
        build_efuse_image(self.mac, self._efuse)
        self._console_log = open(os.path.join(self.workdir, "console.log"), "w")
        self._reader = threading.Thread(target=self._run, name="qemu-console", daemon=True)
        self._launch()
        self._reader.start()

    @property
    def _flash(self) -> str:
        return os.path.join(self.workdir, "flash.bin")

    @property
    def _efuse(self) -> str:
        return os.path.join(self.workdir, "efuse.bin")

    # QEMU binds the web forward and the gdbstub itself, on ports _free_port() found free a moment
    # before, which something else can take meanwhile ("Address already in use" from a busy CI runner).
    # It then exits at once, so look for that and try again on fresh ports.
    PORT_CLASH = re.compile(rb"Address already in use|Failed to find an available port|[Cc]ould not set up host forwarding")
    PORT_RETRIES = 3
    STARTUP_S = 0.5

    def _launch(self) -> None:
        for attempt in range(1, self.PORT_RETRIES + 1):
            self._launch_once()
            if attempt == self.PORT_RETRIES or not self._exited_on_a_port_clash():
                return
            log.warning("QEMU could not bind its ports (attempt %d); trying fresh ones", attempt)
            self.web_port = _free_port()
            if self.gdb_port is not None and gdb_mode() != "wait":
                self.gdb_port = _free_port()

    def _exited_on_a_port_clash(self) -> bool:
        """Whether QEMU exited within STARTUP_S because a port was taken. Any other early exit's output is kept for
        the console reader, which reports it as usual."""
        assert self._proc is not None and self._proc.stdout is not None
        deadline = time.monotonic() + self.STARTUP_S
        while self._proc.poll() is None:
            if time.monotonic() >= deadline:
                return False  # Running: its ports are bound.
            time.sleep(0.02)
        output = self._proc.stdout.read()  # It has exited, and nothing else reads its output yet.
        if self.PORT_CLASH.search(output):
            log.warning("QEMU exited: %s", output.decode("utf-8", errors="replace").strip().splitlines()[-1:])
            return True
        self._early_output = output
        return False

    def _launch_once(self) -> None:
        self._early_output = b""
        args = [
            find_qemu(),
            "-M", "esp32",
            "-m", "4M",  # PSRAM; CONFIG_SPIRAM_BOOT_INIT fails without it
            "-display", "none",
            "-monitor", "none",
            "-serial", "stdio",
            "-serial", "null",
            "-serial", "tcp:127.0.0.1:%d" % self.nextion.port,
            "-drive", "file=%s,if=mtd,format=raw" % self._flash,
            "-drive", "file=%s,if=none,format=raw,id=efuse" % self._efuse,
            "-global", "driver=nvram.esp32.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth,hostfwd=tcp:127.0.0.1:%d-:80" % self.web_port,
            # QEMU's in-place reset leaves interrupt state from the previous boot
            # behind, and the next boot can take an interrupt with no handler
            # (InstrFetchProhibited at PC 0 in esp_timer_init). Exit instead, and
            # let _run() start a fresh QEMU on the same flash: a clean power-on.
            "-no-reboot",
        ]
        # NSPM_QEMU_GDB=1 exposes a gdbstub on a free port; =wait uses :3333 and also halts at
        # reset until gdb attaches.
        if self.gdb_port is not None:
            args += ["-gdb", "tcp:127.0.0.1:%d" % self.gdb_port]
            if gdb_mode() == "wait":
                args += ["-S"]
                log.warning("waiting for gdb on :%d", self.gdb_port)
            else:
                log.info("gdbstub on :%d", self.gdb_port)
        log.info("starting %s", " ".join(args))
        # stdin must stay open: EOF on QEMU's stdio chardev stalls UART0, and
        # with it every task that logs.
        self._proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def dump_cpus(self, path: str, timeout: float = 60) -> Optional[str]:
        """With NSPM_QEMU_GDB set, attach gdb to the running guest and write what each CPU is doing,
        the kernel's locks, and every FreeRTOS task with what it is blocked on (freertos_tasks.py), to
        `path`. For a guest that hangs or deadlocks without a panic.
        Returns the path, or None if there was no stub or guest to attach to."""
        if self.gdb_port is None or not self.running:
            return None
        elf = os.path.join(self.build_dir, "firmware.elf")
        commands = [
            "set pagination off", "set confirm off", "target remote 127.0.0.1:%d" % self.gdb_port,
            "info threads", "thread apply all bt 40",
            "thread 1", "p pxCurrentTCBs[0]->pcTaskName", "p pxCurrentTCBs[1]->pcTaskName",
            # Each CPU's own state, to check against what the scheduler thinks is running.
            "thread 1", "x/3i $pc", "info registers pc ps ccount ccompare0 intenable interrupt",
            "thread 2", "x/3i $pc", "info registers pc ps ccount ccompare0 intenable interrupt",
        ]
        for fmt, expr in (
            # Whether the scheduler is suspended on each CPU.
            ("", "uxSchedulerSuspended"),
            # The flash driver's cross-CPU handshake (components/spi_flash/cache_utils.c).
            ("", "s_flash_op_can_start"), ("", "s_flash_op_complete"), ("", "s_flash_op_cpu"),
            # The spinlocks the scheduler and the cross-core interrupt take (owner: 0xB33FFFFF free,
            # 0xCDCD core 0, 0xABAB core 1), and the pending cross-core reasons.
            ("/x", "xKernelLock"), ("/x", "reason_spinlock"), ("/x", "reason"),
            # picolibc's stdout, shared by every task and line-buffered: bytes written but not yet
            # sent to UART0 (len), and its lock. A log line can be stuck here, unseen on the console.
            ("", "__stdout.len"), ("", "__stdout.off"), ("", "__stdout.pos"),
            ("", "__stdout.buf[0]@(__stdout.len > 0 ? __stdout.len : 1)"),
            ("", "*__stdout.xfile.cfile.file.lock"),
        ):
            commands += ["printf \"%s = \"" % expr, "output%s %s" % (fmt, expr), "echo \\n"]
        commands += [
            "source %s" % os.path.join(os.path.dirname(os.path.abspath(__file__)), "freertos_tasks.py"),
            "nspm-tasks",
            "detach",
        ]
        try:
            gdb, env = find_gdb()

            def run(cmds: List[str]) -> bytes:
                args = gdb + ["-q", "-batch", elf] + [a for c in cmds for a in ("-ex", c)]
                return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, env=env).stdout

            # Is the guest still moving? Sample each CPU's cycle counter and the FreeRTOS tick twice,
            # letting the guest run in between (gdb resumes it on detach). A snapshot alone cannot
            # tell a CPU stopped in the emulator from one that is merely idle.
            clocks = ["thread 1", "p/x $ccount", "thread 2", "p/x $ccount", "p xTickCount"]
            first = _clock_sample(run(commands[:3] + clocks + ["detach"]))
            time.sleep(CLOCK_SAMPLE_GAP)
            out = run(commands[:3] + clocks + commands[3:])
            out = _clock_report(first, _clock_sample(out)).encode() + out
        except (OSError, subprocess.TimeoutExpired) as e:
            out = ("gdb failed: %s" % e).encode()
        with open(path, "wb") as f:
            f.write(out)
        return path

    def stop(self) -> None:
        self._stopping = True
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
        if self._proc is not None and self._proc.poll() is not None and not self._relaunching:
            raise PanelCrashed("QEMU exited with %s\n%s" % (self._proc.returncode, self.tail(40)))

    def tail(self, n: int = 40) -> str:
        with self._cond:
            return "\n".join(self.lines[-n:])

    def _run(self) -> None:
        """Read the console; when the firmware resets (QEMU exits 0 under -no-reboot), boot it again."""
        while True:
            self._read_console()
            assert self._proc is not None
            code = self._proc.wait()
            if self._stopping or code != 0 or self.crash is not None:
                return
            with self._cond:
                self._relaunching = True
            log.info("firmware reset; starting QEMU again on the same flash")
            try:
                self._launch()
            finally:
                with self._cond:
                    self._relaunching = False

    def _read_console(self) -> None:
        assert self._proc is not None and self._proc.stdout is not None
        # What _exited_on_a_port_clash() read from a QEMU that exited at once, then the rest.
        early = self._early_output.splitlines(keepends=True)
        self._early_output = b""
        for raw in itertools.chain(early, self._proc.stdout):
            line = _ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip("\r\n")
            if self._console_log is not None:
                self._console_log.write(line + "\n")
                self._console_log.flush()
            with self._cond:
                self.lines.append(line)
                if self.crash is None and any(p.search(line) for p in CRASH_PATTERNS):
                    self.crash = line
                reset = ROM_RESET.search(line)
                if reset:
                    self.resets.append(reset.group(1))
                self._cond.notify_all()
            if NEXTION_POWER_CYCLE.search(line):
                self.nextion.power_cycle()
                if self.boot_display:
                    threading.Timer(NEXTION_BOOT_DELAY, self._display_boot).start()
        with self._cond:
            self._cond.notify_all()

    def _display_boot(self) -> None:
        try:
            self.nextion.boot()
        except (OSError, RuntimeError) as e:
            log.warning("could not send NSPM flag: %s", e)

    @property
    def boots(self) -> int:
        with self._cond:
            return len(self.resets)

    def wait_for_reboot(self, boots_before: int, timeout: float = 120) -> None:
        """Wait until the firmware has restarted since `boots` was `boots_before`."""
        self.wait_until(lambda: self.boots > boots_before, timeout, "panel reboot")

    # ---- helpers -----------------------------------------------------------

    def wait_until(self, predicate: Callable[[], bool], timeout: float = 30, what: str = "condition") -> None:
        deadline = time.monotonic() + timeout
        while not predicate():
            self.check_alive()
            if time.monotonic() > deadline:
                raise TimeoutError("%s not met within %.0fs" % (what, timeout))
            time.sleep(0.1)
