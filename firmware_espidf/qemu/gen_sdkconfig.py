# PlatformIO pre-script for [env:qemu]: build sdkconfig.qemu from
# sdkconfig.original_sonoff plus qemu/sdkconfig.qemu.defaults.
#
# Regenerated whenever either input is newer than the output. ESP-IDF rewrites
# sdkconfig.qemu during configure, which leaves it newer than both inputs, so
# an unchanged overlay does not force a reconfigure on every build.
import os
import re

Import("env")  # noqa: F821

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
base = os.path.join(project_dir, "sdkconfig.original_sonoff")
overlay = os.path.join(project_dir, "qemu", "sdkconfig.qemu.defaults")
output = os.path.join(project_dir, "sdkconfig.qemu")

SYMBOL = re.compile(r"^(?:# )?(CONFIG_[A-Za-z0-9_]+)(?:=| is not set)")


def symbols(lines):
    return {m.group(1) for m in (SYMBOL.match(line) for line in lines) if m}


def stale():
    if not os.path.exists(output):
        return True
    out_mtime = os.path.getmtime(output)
    return any(os.path.getmtime(p) > out_mtime for p in (base, overlay))


if stale():
    with open(base) as f:
        base_lines = f.read().splitlines()
    with open(overlay) as f:
        overlay_lines = [line for line in f.read().splitlines() if SYMBOL.match(line)]
    overridden = symbols(overlay_lines)
    kept = [line for line in base_lines if not (SYMBOL.match(line) and SYMBOL.match(line).group(1) in overridden)]
    with open(output, "w") as f:
        f.write("\n".join(kept + ["", "# --- qemu/sdkconfig.qemu.defaults ---"] + overlay_lines) + "\n")
    print("gen_sdkconfig: wrote %s" % output)
