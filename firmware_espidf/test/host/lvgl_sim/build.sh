#!/bin/sh
# Builds and runs the LVGL host simulator (main.cpp): every page of lib/LvglRenderer drawn on a US
# and an EU screen, with what each frame costs on the terminal link. Needs LVGL's source, which
# `pio run` fetches into managed_components/ (or set LVGL_DIR).
#
#   test/host/lvgl_sim/build.sh [OUT_DIR] [art]     # PPMs, and PNGs if Pillow is importable; "art": only the HD art
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../../.."
lib="$root/lib/LvglRenderer"
lvgl="${LVGL_DIR:-$root/managed_components/lvgl__lvgl}"
build="${TMPDIR:-/tmp}/nspm-lvgl-sim"
out="${1:-$build/out}"
[ -f "$lvgl/lvgl.h" ] || { echo "no LVGL at $lvgl; run pio run -e qemu first, or set LVGL_DIR" >&2; exit 2; }
mkdir -p "$build/obj" "$out"

cflags="-O2 -g -DNSPM_HD_FONTS -DNSPM_HD_ART -DLV_CONF_INCLUDE_SIMPLE -I$here -I$lvgl -I$lvgl/src -I$lib -I$root/lib/NextionTerminal"
# LVGL: compiled once, then only what changed (make-style timestamps via find -newer).
find "$lvgl/src" -name '*.c' | while read -r src; do
  obj="$build/obj/$(echo "$src" | sed "s|$lvgl/||; s|/|_|g").o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$here/lv_conf.h" -nt "$obj" ]; then echo "$src $obj"; fi
done | xargs -r -P "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" -n 2 sh -c 'cc '"$cflags"' -c "$0" -o "$1"' 2>"$build/lvgl-build.log"

# Every font in lib/LvglRenderer, or FONT_DIR's version of it where FONT_DIR has one.
fonts="${FONT_DIR:-$lib}"
rm -f "$build"/nspm_font_*.o
for src in "$lib"/nspm_font_*.c; do
  f=$(basename "$src" .c)
  [ -f "$fonts/$f.c" ] && src="$fonts/$f.c"
  cc $cflags -c "$src" -o "$build/$f.o" 2>>"$build/lvgl-build.log"
done
# Every screen and page: all of lib/LvglRenderer but the device glue.
views=$(ls "$lib"/*.cpp | grep -v '/LvglRenderer.cpp$')
c++ -std=c++17 $cflags "$here/main.cpp" "$here/Pages.cpp" $views "$build"/nspm_font_*.o "$build"/obj/*.o -lm \
  -o "$build/lvgl_sim" 2>>"$build/lvgl-build.log" || { cat "$build/lvgl-build.log" >&2; exit 1; }
"$build/lvgl_sim" "$out" ${2:-}

python3 -c "import PIL" 2>/dev/null && python3 - "$out" <<'EOF'
import glob, os, sys
from PIL import Image
for p in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(p).save(p[:-4] + ".png")
    os.remove(p)
EOF
echo "screens in $out"
