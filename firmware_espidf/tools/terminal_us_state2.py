#!/usr/bin/env python3
"""Fix nspanel-terminal's terminal-us.tft for real US panels (as of nspanel-terminal f0c1394).

On a US NSPanel (Office, 2026-11-14) the stock runtime enters touch_j at project state **2**, not 1
as the US profile assumes, so the stock state selector (native 0xC06E: ldrb r0,[r0,#1]; cmp r0,#1;
bne) never reaches helper 0 and the loader's own gate would refuse too. A diagnostic loader reported
width 320, height 480, state 2, DMA/UART idle.

This builds: the official terminal-us.tft, with the EU profile's 0xC072 patch (cmp r0,#2), the
official receiver bytes, terminal_loader.c rebuilt with NSPANEL_ENTRY_STATE=2, and a native version
of its own (the Nextion updater keeps native code whose version matches the installed one, so a
changed image must be numbered apart).

usage: terminal_us_state2.py <nspanel-terminal checkout> <out.tft> [version, default 98] [clang, default clang-21]
"""
import subprocess, sys, tempfile, shutil
from pathlib import Path

repo, out_path = Path(sys.argv[1]), Path(sys.argv[2])
version = int(sys.argv[3]) if len(sys.argv) > 3 else 98
cc = sys.argv[4] if len(sys.argv) > 4 else "clang-21"
sys.path.insert(0, str(repo / "tools"))
import build_tft as b  # noqa: E402
from tft_crc import crc  # noqa: E402
from panel_profiles import PROFILES  # noqa: E402

prof = PROFILES["us"]
official = (repo / "firmware/terminal-us.tft").read_bytes()
helper = official[b.HELPER_BUNDLE + b.HELPER_OFFSET : b.HELPER_BUNDLE + b.HELPER_OFFSET + b.HELPER_SLOT]
pre = official[b.PREAMBLE_START : b.PREAMBLE_END]
# The receiver's length: the one whose CRC and size the official loader embeds.
for n in range(len(pre.rstrip(b"\xff")), len(pre.rstrip(b"\xff")) + 8):
    if crc(pre[:n]).to_bytes(4, "little") in helper and n.to_bytes(4, "little") in helper:
        break
else:
    sys.exit("receiver length not found")
program = pre[:n]
with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    for f in (repo / "firmware/src").glob("*.h"):
        shutil.copy(f, work)
    (work / "terminal_receiver_meta.h").write_text(
        f"#define POOL_BYTES {n}u\n#define POOL_CRC 0x{crc(program):08x}u\n#define POOL_ENTRY 0x{b.PROGRAM_BASE + 1:08x}u\n")
    defines = [d if not d.startswith("-DNSPANEL_ENTRY_STATE=") else "-DNSPANEL_ENTRY_STATE=2" for d in prof.defines]
    subprocess.run([cc, *b.COMPILE_FLAGS, *defines, "-I", str(work), "-c", str(repo / "firmware/src/terminal_loader.c"),
                    "-o", str(work / "loader.o")], check=True)
    loader, _ = b.assemble(work / "loader.o", b.LOADER_BASE)
tft = b.patch_native_image(official, version, ((0xC072, bytes.fromhex("02")),))
tft = b.patch_package(tft, program, loader)
out_path.write_bytes(tft)
print(f"{out_path}: native {b.native_version(tft)}, receiver {n} B (official), loader {len(loader)} B, state 2")
