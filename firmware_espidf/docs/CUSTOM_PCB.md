# custom_pcb: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B

The `custom_pcb` environment (`-DBOARD_CUSTOM`) builds for the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm)
(examples and schematics: [waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B)).
The manager still knows it as model `custom`. The earlier ESP32-S3 custom PCB is no longer built.

| | |
|---|---|
| SoC | ESP32-P4 (rev 3.x, RISC-V, 400 MHz), 32 MB QIO flash, 32 MB PSRAM (hex, 200 MHz) |
| Wi-Fi | ESP32-C6 co-processor over SDIO (ESP-Hosted 3.0.10 + esp_wifi_remote 1.6.5, the versions of Waveshare's factory app). SDIO CLK 18, CMD 19, D0-D3 14-17, C6 reset 54 (`ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`) |
| Display | 4" 720x720 MIPI-DSI, 2 lanes at 480 Mbps (D-PHY on LDO VO3, 2.5 V), reset GPIO 27; ST7703 or JD9365DA depending on the unit, probed at boot. Backlight GPIO 26 (PWM, inverted). GT911 touch on I2C port 1 (SDA 7, SCL 8), reset GPIO 23, INT not wired (polled) |
| Microphones | ES7210 ADC (I2C 0x40, the same bus as touch: `lib/BoardI2c`), I2S 0 TDM RX at 16 kHz, MCLK 13, BCLK 12, WS 10, DIN 11. Slots: MIC1, MIC3 (the ES8311's output looped back, the echo reference), MIC2, MIC4 (not connected). Started on first use (`lib/VoiceCapture`); for now only the temporary `GET /rec/test.wav?secs=5&ch=all\|1\|2\|ref&gain=30` uses it. The ES8311 (0x18) and the speaker amplifier (enable GPIO 53) are not driven |
| Buttons, relays | None. The relays are state only, kept and reported over MQTT as on the Sonoff |
| Temperature | None reported (the P4's die sensor is not room temperature) |
| Console | UART0, on the board's USB-UART USB-C port, 115200 |

## The display

`lib/DsiDisplay` drives it, and LVGL is always the UI (`display_mode: LVGL`). LVGL draws the whole
720x720 screen, with the pages laid out portrait at 1.5x the NSPanel's sizes. Every length in the views
is in NSPanel pixels and goes through `Theme::px()` (or is a `Theme::Px` constant). The fonts are a
second set at 1.5x their size (`nspm_font_hd_*.c`, from `tools/gen_fonts.sh`), compiled only where
`NSPM_HD_FONTS` is defined, so the NSPanel's flash doesn't carry them. On the NSPanel `px()` returns its
argument, and its pages are drawn exactly as before.

LVGL draws straight into the panel's two frame buffers (direct mode), into the one the panel is not
sending. After each refresh `DsiDisplay::show()` hands that one to the panel, which switches to it at
the next frame (no tearing), and LVGL copies what it redrew into the other before drawing there.
Touches are in the panel's coordinates. The page backgrounds (320x480 art) are drawn scaled to the
width, their bottoms on the screen's. `test/host/lvgl_sim/build.sh` draws every page this way too, as
`hd-*`.

| | |
|---|---|
| Panel | `DsiPanel.c`: Waveshare's ID probe and the two controllers' timings (and the JD9365's init table) from their board definition, `waveshare/waveshare_boards` 0.3.7. The boot log says which: `DsiDisplay: Panel controller: ST7703.` |
| Brightness | The manager's screen and screensaver brightness, 0-100 %, as PWM duty; 0 turns the backlight off. Dark until the first frame is shown |
| Screenshots | `GET /screenshot` works as on the NSPanel, at 720x720 |
| GUI update | Refused: there is no TFT to update |

## What differs from the Sonoff build

- **Platform.** The stock `espressif32` platform's builder has no ESP32-P4, so this environment uses
  pioarduino's (61.04.00, ESP-IDF 6.1.1; needs PlatformIO Core 6.2). `fix_forced_includes.py` works
  around a pioarduino builder bug that drops ESP-Hosted's forced `-include eh_host_port_master_config.h`
  (see the script).
- **MAC.** The panel's MAC is the C6's Wi-Fi station MAC, read over ESP-Hosted (`esp_wifi_get_mac()`) in
  `WiFiManager::init()`, which now runs before `ConfigManager::create_default()`. The P4's own eFuse MAC
  is not used.
- **Nextion.** `Nextion::present()` is false: no UART or GPIO is claimed, commands are dropped and a TFT
  update is refused. `LvglUi::init()` starts `DsiDisplay` instead of looking for a terminal TFT.
  GPIO 14-19 are the C6's SDIO bus, so none of the old board's pins there (buttons 14/15, relays 16/17,
  the BME280's I2C clock on 18) may be driven.
- **EventLoopWatchdog.** Logs a stall, but no per-task backtraces: `esp_backtrace_print_all_tasks()` is
  Xtensa-only.
- **Partitions.** `partitions_p4.csv`: same names, 4 MB app slots, 4 MB LittleFS.
- **sdkconfig.** `sdkconfig.custom_pcb` is the P4's defaults with `sdkconfig.original_sonoff`'s settings;
  after changing the Sonoff's, run `tools/sync_sdkconfig_custom_pcb.py` (see its docstring).

## Build and flash

```
pio run -e custom_pcb
pio run -e custom_pcb -t upload --upload-port <port>
```

The board has two USB-C ports:

- **USB-UART** (a WCH CH343, `1a86:55d3`; on macOS `/dev/cu.usbmodem<serial>`) is UART0: the console
  and the one to use. Flash through it, and read the log on it (`pio device monitor -b 115200 -p <port>`).
  Its DTR/RTS lines do not reach the chip's EN and BOOT, so esptool cannot reset the chip into the ROM
  loader, nor back into the app afterwards: put the board in download mode by hand (BOOT held while it
  powers up or resets) before flashing, and power-cycle it afterwards to run the app. An open serial
  monitor does not reset it either, so to see a boot from the start, open the port first and then
  power the board.
- **USB-OTG** (`303a:0012` in the ROM loader) also flashes, but after the upload's watchdog reset the
  chip comes back up in the ROM loader, not the app: unplug it to boot. The app prints nothing there.

Once a config is saved, the log level is the manager's (warnings by default), so the INFO lines below
show only on a first boot, or with the log level raised in the manager. The boot log shows
`WiFiManager: Wi-Fi MAC ...` (the C6's), ESP-Hosted's view of the C6 firmware,
`DsiDisplay: Panel controller: ...` and the touch controller's address. A crash shows as
`abort() was called at PC ...` followed by `Rebooting...`; decode the addresses with
`riscv32-esp-elf-addr2line -pfiaC -e .pio/build/custom_pcb/firmware.elf <addresses>`
(in `~/.platformio/packages/toolchain-riscv32-esp/bin`).

The ESP32-C6 keeps the firmware it shipped with; erasing or flashing the P4 does not touch it. On the
first board tried, it reports ESP-Hosted `coprocessor=0.0.0` and the log says
`major version mismatch — OTA coprocessor from host`; Wi-Fi (station and the setup AP) works
regardless. If it stops working, the C6 needs ESP-Hosted 3.0.10 slave firmware (Waveshare's factory
app installs it). Waveshare's factory image is
`firmware/ESP32-P4-WIFI6-Touch-LCD-4B_FactoryOnly_*.bin` in their repo.

The P4's bootloader starts at 0x2000, so it must stay under 0x6000 bytes (the partition table is at
0x8000): `sdkconfig.custom_pcb` builds it for size, not speed as the Sonoff's is. If it grows past that,
the upload fails with `Detected overlap at address: 0x8000`.
