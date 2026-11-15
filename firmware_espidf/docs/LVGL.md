# LVGL as the panel's UI

On a panel whose display runs an [nspanel-terminal](https://github.com/ptaru/nspanel-terminal) TFT,
the firmware draws every page itself with [LVGL](https://lvgl.io) 9.6 and streams the pixels to the
display at 4 Mbaud. On NSPanel Manager's own TFT nothing changes: the Nextion pages run as before.
NSPM's pages, data, navigation and manager protocol are unchanged; only the drawing moves.

## How it fits together

| Part | Where | Does |
|---|---|---|
| Mode | `lib/LvglUi/LvglUi.cpp` (`init()`) | LVGL is the UI when the display reports a terminal TFT (`NextionTerminal::is_receiver()`), sent no NSPM startup flag, and `lvgl_ui` is on |
| Page adapters | `lib/Pages/*` (`LvglUi::active()` branches) | Where a page would drive the Nextion, it hands a view's state to `LvglUi::show()`; view actions call the same handlers the Nextion touch events did |
| Host | `lib/LvglUi/` | Screensaver timeout and wake, swipe-down menu, touch sound, backlight, album art fetch (`LvglArt`), Latin-1 text (`LvglText`) |
| Views | `lib/LvglRenderer/*View.cpp`, `Theme`, `Ui`, `Background` | The screens, free of ESP-IDF (the host simulator builds them) |
| Renderer | `lib/LvglRenderer/LvglRenderer.cpp` | LVGL's task, draw buffer, touch input, boot screen |
| Transport | `lib/NextionTerminal/` | The terminal session; see its README |
| Nextion | `lib/Nextion/` | In LVGL mode: commands and queries dropped (`set_commands_blocked`), pages redirected to LVGL, brightness to the receiver |

Threads: LVGL runs on its own task (core 1) and nothing else calls it; other tasks `post()` work to
it. View actions run there too, so they `LvglUi::defer()` anything that publishes or changes pages.
The terminal task (core 0) encodes and sends while LVGL renders the next band.

## Commands

On `nspanel/<MAC>/command`:

| Command | Effect |
|---|---|
| `{"command": "lvgl_ui", "enable": false}` | Keep the Nextion pages whatever the TFT (saved; reboots). A kill switch that needs no TFT reinstall; on a terminal TFT the display then shows the TFT's own "waiting" page, but the panel still takes MQTT, firmware and TFT updates |
| `{"command": "lvgl_background", "preset": "ocean"}` | Page background: `none`, `ocean`, `dusk`, `aurora`, `peaks` (saved; applied at once) |
| `{"command": "lvgl_theme", "theme": "light"}` | Colours (`Theme::Palette`): `auto` (the default: the background's own palette, `dark` without one), `dark`, `light`, `ocean`, `dusk`, `aurora`, `peaks` (saved; applied at once). `light` draws no background, as the art is dark; the background setting is kept for when the theme changes back |
| `{"command": "lvgl_tap", "x": 160, "y": 20, "hold_ms": 100}` | A touch through the LVGL path, for testing without a finger; `x2`/`y2` make it a swipe |

## Screenshots

`GET http://<panel>/screenshot` on the panel's own web server returns the screen as LVGL draws it,
upright and in screen coordinates (320x480 US, 480x320 EU), whatever the receiver's rotation. No
auth, like the rest of that server. There is no MQTT command for it yet.

```
curl -o office.png http://<panel-ip>/screenshot
curl -o office.bmp 'http://<panel-ip>/screenshot?format=bmp'
```

| `format=` | Body | Size (320x480) |
|---|---|---|
| `png` (default) | 8-bit RGB, deflate (ROM `tdefl`, fastest setting), chunked | typically 5-50 KB |
| `bmp` | 16-bit `BI_BITFIELDS` (565 masks), top-down, chunked | 307,266 B |
| `raw` | RGB565 little-endian, row by row, with `Content-Length` | 307,200 B |

Headers: `X-Width`, `X-Height`, `X-Capture-Ms` (the GUI task's time on it), `X-Display-Streaming`
(`1` when the receiver was streaming and nothing was left to repaint, so the display shows exactly
this; `0` while the terminal restarts or after a band failed), `Cache-Control: no-store`,
`Content-Disposition: inline; filename="<hostname>.<format>"`, and for `raw`
`X-Pixel-Format: RGB565LE`.

| Status | When |
|---|---|
| 200 | The image |
| 400 | `format` is not `png`, `bmp` or `raw` |
| 409 | The panel shows NSPM's Nextion pages (`LvglUi` not active): the Nextion draws those itself and nothing can be read back |
| 503 | Another screenshot is under way, no PSRAM for the 300 KB copy, the GUI queue is full, or the GUI task took over 2 s (`Retry-After: 1`) |

How (`LvglRenderer::capture()`, `lib/LvglUi/LvglScreenshot.cpp`, `WebManager`): neither the ESP32 nor
the receiver keeps a framebuffer, so the screen is rendered once more on request. The handler
allocates a 300 KB PSRAM buffer and posts the job to the GUI task, which runs it right after its
refresh: first it sends whatever is still due to the display, then it invalidates the whole screen
and refreshes again with `_flush` copying each band into the buffer instead of calling
`NextionTerminal::draw()`. Nothing runs between the two passes (no animation step, no timer, no
posted work), so the copy is what the display shows, and the display never sees the second pass.
The GUI task stalls for about one full render; the encoding and the network run on the web
server's task. Whichever of the two lets go of the capture last frees it, so a request that times
out never frees memory the GUI task is still writing. PNG uses the ESP32 ROM's miniz `tdefl` (no
flash), its ~160 KB of state in PSRAM for the request only, with the PNG framing written by hand
and one IDAT chunk per deflate block. Nothing is held between requests. Each request logs
`WebManager: Screenshot <format> WxH, N bytes: capture .. ms (GUI task .. ms), encode .. ms, send ..
ms` with the web server task's unused stack.

## Putting a panel on it

1. Stage firmware with LVGL mode. Panels on NSPM's TFT ignore it.
2. Build the TFT: nspanel-terminal's tools and base TFTs, then `tools/terminal_us_state2.py` for a
   US panel (see the NextionTerminal README), and upload it to a TFT slot on the manager.
3. Point the panel's TFT setting at that slot, then update its GUI. To roll back, point it at NSPM's
   TFT again and update; the firmware follows the TFT.

## Performance (Office, US receiver)

| | Plain | Ocean background |
|---|---|---|
| Page change (build, layout, repaint) | ~229 ms | ~241 ms |
| Full repaint | ~105 ms | ~118 ms |

The transfer is bound by the receiver (~1.5 Mpx/s through its 512-byte credit window), not the
ESP32. On the ESP32 side, PSRAM and flash share one cache at 40 MHz, so anything touching them is
slow. That is why LVGL draws into 20 lines of internal RAM, its per-pixel code is in IRAM
(`src/lvgl_iram.lf`), and LVGL is built `-O2` while the rest is `-Os`. Things tried and dropped:
sending only changed pixels (a PSRAM shadow cost more than it saved), LVGL's small allocations in
internal RAM (no gain). Not tried: PSRAM and flash at 80 MHz. It needs a panel with serial access or
OTA rollback, as a wrong clock fails at boot.

Album art is fetched after the page is drawn (180x180 RGB565 from the manager) and costs ~0.2 s on
the wire. The backgrounds are designed for the link: colour changes only down the screen or at a few
edges per row, so they cost 3-13% more bytes than a plain screen (a photo costs 3-5 times as much).

## Flash

The app slot is 1,769,472 bytes. With LVGL the image is ~1,590,000 bytes (~10% free). What keeps it
there:

- `FMT_OPTIMIZE_SIZE=2` (root `CMakeLists.txt`): ~152 KB.
- Fonts are subsets (`tools/gen_fonts.sh`): only the glyphs the pages use, 1-2 bpp.
- LVGL widgets and Montserrat sizes the views don't use are off in `sdkconfig.original_sonoff`; the
  default font is the 2 KB UNSCII 8, as every label sets its own.
- The backgrounds are generated at run time into PSRAM, not stored.

Further room if needed, measured as builds but not run on a panel: trimming mbedTLS options
(~17 KB), Wi-Fi extras off (~15 KB), `MBEDTLS_AES_FEWER_TABLES` (~6 KB).

## Simulator

`test/host/lvgl_sim/build.sh [OUT]` draws every view with mock data (`Pages.cpp`) on US and EU
screens, plain and over each background, and writes the frames as images with each frame's cost
on the wire (`costs.csv`). `NSPM_THEME=light` (or any palette) draws it all in that palette. It needs LVGL's source, which `pio run` fetches into
`managed_components/`.
