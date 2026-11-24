# NextionTerminal

Streams RGB565 rectangles to the display at 4 Mbaud through the receiver that a patched
[nspanel-terminal](https://github.com/ptaru/nspanel-terminal) TFT adds to the Nextion runtime
(protocol 8: COBS records with a header CRC, PackBits pixels, a 512-byte credit window, touch in the
replies). `LvglUi` starts a session at boot when the display runs such a TFT, and `LvglRenderer`
draws through it.

`TerminalProtocol.hpp`, `TerminalStreamEngine.hpp` and `TerminalGapSplitter.hpp` are copied from
nspanel-terminal's ESPHome component (MIT, see `LICENSE.nspanel-terminal`); only the namespace and
include names differ. `NextionTerminal.cpp` is the ESP-IDF host: the session setup, and the pump
loop that `nspanel_terminal.cpp` runs in ESPHome's main loop.

## Requirements

- The display runs an nspanel-terminal TFT: its `connect` reply is `NX4832F035_011C`, MCU code
  61744, native version 94-127 (`is_receiver()`). Nothing else is entered: the setup ends in
  `touch_j`, which on another runtime starts touch calibration. NSPanel Manager's own TFTs report 72
  or 128.
- US and EU panels have the same display, so one TFT fits both. nspanel-terminal's
  `terminal-us.tft` does not start on a US panel (its runtime enters `touch_j` at state 2, not 1):
  build a fixed image with `tools/terminal_us_state2.py`, which also gives it a native version of its
  own (the Nextion updater keeps native code whose version matches the installed one).
- A TFT's startup code sets the display's baud rate: NSPM's sets 115200, the terminal TFTs leave
  9600. A session tries the link's rate, then 9600 and 115200, for its setup; the receiver then
  moves to 4 Mbaud itself.
- A terminal TFT has none of NSPM's HMI pages: the panel shows only what LVGL draws. On NSPM's own
  TFT the firmware does not open a session: it shows the legacy "Display update needed" screen.

## Receivers (`options_for()`)

- **US** (the default; 320x480, all 480 lines): its runtime draws upside down on a US panel, so
  picture and touch are turned 180 degrees. Full repaint ~100 ms on Office.
- **EU** (`terminal-eu.tft`, 454x320 landscape): on a US panel the screen is turned 90 degrees and
  26 lines are not drawn (the receiver rejects columns 454-479); touch is offset by 26, as the touch
  controller counts all 480 lines. Full repaint ~124 ms.

The rectangles of one LVGL refresh go as a burst: every one but the last ends with `END x=0`, so
the receiver confirms only the last. Every 10 s while drawing, the log has the rectangles, the
bytes on the wire and the rate.

## Session

1. `Nextion::terminal_acquire()`: state `TERMINAL`, other writers refused, frame reading and
   pattern detection off.
2. `connect` and the identity check.
3. `bkcmd=0`, `sys0=4000000`, `sys1=1`, `sys2=<session>`, `touch_j`, each byte 190 µs apart and
   each command 40 ms apart, as the nspanel-terminal host does; then the UART moves to 4 Mbaud and
   the receiver answers `READY` (or `READY_BRIGHTNESS`: live backlight, `set_brightness()`).
4. Streaming: `draw()` copies (and turns) LVGL's band, hands it to the terminal task and returns
   without waiting, so LVGL renders the next band meanwhile; the next `draw()` waits for it. The
   task sends `RECT`, `DATA`, `END` within the credit window and heartbeats every 80 ms when idle.
5. `stop()`: `EXIT`, then `Nextion::terminal_release(true)` power-cycles the display and connects
   as at boot. A failed session ends the same way and is retried.

Unlike nspanel-terminal, it does not save and restore `page`, `bkcmd` and `sys0..2`: the power cycle
resets them.
