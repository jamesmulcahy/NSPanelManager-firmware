#!/bin/sh
# Regenerates lib/LvglRenderer's fonts from LVGL's Montserrat (see tools/lv_font_subset.py).
#   tools/gen_fonts.sh [BODY_BPP [LARGE_BPP]] [OUT_DIR]     # default 2 and 2, into lib/LvglRenderer
# Fewer antialiasing levels mean less flash and longer runs of one colour on the wire. 2 bpp (four
# levels) for all text: at 1 bpp the 14 px body text looked jagged, and 2 bpp costs ~2 KB of flash
# and ~4% more bytes per page redraw.
# The symbols are the LV_SYMBOL_* the screens use: wifi, home, loop, right, settings, list, image,
# and ok (the checkbox's tick), plus the degree sign.
#
# The page icons (nspm_font_icon_*.c, nspm_icons.h) are Material Design Icons rasterised by
# tools/lv_icon_font.py from the MDI webfont the manager's web UI ships (MDI_TTF, default: the
# NSPanelManager checkout next to this one). That needs Pillow with FreeType: PYTHON defaults to
# qemu/.venv's. Only the icons each size draws go into the 40 and 64 px fonts.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/.."
fonts="${LVGL_DIR:-$root/managed_components/lvgl__lvgl}/src/font"
body_bpp="${1:-2}"
large_bpp="${2:-2}"
out="${3:-$root/lib/LvglRenderer}"
symbols='\u00b0\uf1eb\uf015\uf079\uf054\uf013\uf00b\uf03e\uf00c'
python3 -I "$here/lv_font_subset.py" --bpp "$body_bpp" --fallback nspm_font_body_latin "$fonts/lv_font_montserrat_14.c" nspm_font_body "ASCII$symbols" > "$out/nspm_font_body.c"
python3 -I "$here/lv_font_subset.py" --bpp "$large_bpp" --fallback nspm_font_title_latin "$fonts/lv_font_montserrat_20.c" nspm_font_title "ASCII\u00b0" > "$out/nspm_font_title.c"
python3 -I "$here/lv_font_subset.py" --bpp "$large_bpp" "$fonts/lv_font_montserrat_48.c" nspm_font_clock_48 "0123456789: APM-.F\u00b0" > "$out/nspm_font_clock_48.c"
python3 -I "$here/lv_font_subset.py" --bpp "$large_bpp" "$fonts/lv_font_montserrat_32.c" nspm_font_large "0123456789:.-/% CFAPM\u00b0" > "$out/nspm_font_large.c"

python="${PYTHON:-$root/qemu/.venv/bin/python}"
mdi="${MDI_TTF:-$root/../../NSPanelManager/docker/web/nspanelmanager/web/static/mdi/fonts/materialdesignicons-webfont.ttf}"
[ -f "$mdi" ] || mdi="$HOME/nspanel/NSPanelManager/docker/web/nspanelmanager/web/static/mdi/fonts/materialdesignicons-webfont.ttf"
icons() { "$python" -I "$here/lv_icon_font.py" --table "$here/nspm_icons.txt" "$@"; }
icons --header > "$out/nspm_icons.h"
# What the built-in Montserrat lacks, for names from Home Assistant: Latin-1 (Café, Björk, ×), dashes,
# typographic quotes ("James's Music" with U+2019), bullet and ellipsis; the euro and rupee signs for the energy card. The fonts above fall back on these.
montserrat="${MONTSERRAT_TTF:-${LVGL_DIR:-$root/managed_components/lvgl__lvgl}/scripts/generators/built_in_font/Montserrat-Medium.ttf}"
latin=00A1-00FF,2013-2014,2018-201E,2022,2026,20AC,20B9
"$python" -I "$here/lv_icon_font.py" --ttf "$montserrat" --size 14 --bpp "$body_bpp" --codes "$latin" nspm_font_body_latin > "$out/nspm_font_body_latin.c"
"$python" -I "$here/lv_icon_font.py" --ttf "$montserrat" --size 20 --bpp "$large_bpp" --codes "$latin" nspm_font_title_latin > "$out/nspm_font_title_latin.c"
# The screensaver's clock on a portrait screen: larger than LVGL's largest built-in size, so
# rasterised from the TTF. Digits, colon and dash: 88 px still fits "10:00 PM" across the card.
"$python" -I "$here/lv_icon_font.py" --ttf "$montserrat" --size 88 --bpp "$large_bpp" --codes 002D,0030-003A nspm_font_clock_88 > "$out/nspm_font_clock_88.c"
icons --ttf "$mdi" --size 24 --bpp "$large_bpp" nspm_font_icon_24 > "$out/nspm_font_icon_24.c"
# Tiles, transport and big buttons; the empty calendar card; the energy card's lights.
icons40=chevron-left,chevron-right,ceiling-light,lamp,lamps,lightbulb,lightbulb-outline,lightbulb-group,toggle-switch,toggle-switch-off-outline,power-socket-us,fan,gesture-tap-button,window-shutter,window-shutter-open,blinds,garage,thermostat,television,coffee-maker,sofa,speaker,palette,palette-swatch,play,pause,skip-previous,skip-next,shield-home,shield-off,weather-night,spray-bottle,restart,music,arrow-up,arrow-down,stop,calendar,lightbulb-on,timer-outline,microphone
icons --ttf "$mdi" --size 40 --bpp "$large_bpp" --icons "$icons40" nspm_font_icon_40 > "$out/nspm_font_icon_40.c"
# The current weather, the cleaning screen, the album art placeholder and the energy card's power.
icons64=weather-sunny,weather-partly-cloudy,weather-cloudy,weather-rainy,weather-partly-rainy,weather-pouring,weather-snowy,weather-lightning,weather-fog,weather-night,weather-night-partly-cloudy,weather-windy,spray-bottle,music,lightning-bolt
icons --ttf "$mdi" --size 64 --bpp "$large_bpp" --icons "$icons64" nspm_font_icon_64 > "$out/nspm_font_icon_64.c"

# The custom PCB's 720x720 screen (Theme::scale() 1.5): every font above at 1.5x its size, rasterised from
# the TTFs (LVGL has no Montserrat 21, 30 or 72), with the Latin-1 extras and LVGL's symbols in the same
# font rather than a fallback. Only builds that define NSPM_HD_FONTS compile them (the custom PCB, the
# simulator): the NSPanel's flash has no room for them.
hd() { "$python" -I "$here/lv_icon_font.py" --guard NSPM_HD_FONTS "$@"; }
fa="${LVGL_DIR:-$root/managed_components/lvgl__lvgl}/scripts/generators/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
sym=f1eb,f015,f079,f054,f013,f00b,f03e,f00c
hd --ttf "$montserrat" --size 21 --bpp "$body_bpp" --codes "0020-007E,$latin" --sym-ttf "$fa" --sym-codes "$sym" nspm_font_hd_body > "$out/nspm_font_hd_body.c"
hd --ttf "$montserrat" --size 30 --bpp "$large_bpp" --codes "0020-007E,$latin" nspm_font_hd_title > "$out/nspm_font_hd_title.c"
hd --ttf "$montserrat" --size 48 --bpp "$large_bpp" --codes 0020,0025,002D-003A,0041,0043,0046,004D,0050,00B0 nspm_font_hd_large > "$out/nspm_font_hd_large.c"
hd --ttf "$montserrat" --size 72 --bpp "$large_bpp" --codes 0020,002D-002E,0030-003A,0041,0046,004D,0050,00B0 nspm_font_hd_clock > "$out/nspm_font_hd_clock.c"
hd --ttf "$montserrat" --size 132 --bpp "$large_bpp" --codes 002D,0030-003A nspm_font_hd_big_clock > "$out/nspm_font_hd_big_clock.c"
hd --table "$here/nspm_icons.txt" --ttf "$mdi" --size 36 --bpp "$large_bpp" nspm_font_hd_icon_36 > "$out/nspm_font_hd_icon_36.c"
hd --table "$here/nspm_icons.txt" --ttf "$mdi" --size 60 --bpp "$large_bpp" --icons "$icons40" nspm_font_hd_icon_60 > "$out/nspm_font_hd_icon_60.c"
hd --table "$here/nspm_icons.txt" --ttf "$mdi" --size 96 --bpp "$large_bpp" --icons "$icons64" nspm_font_hd_icon_96 > "$out/nspm_font_hd_icon_96.c"
