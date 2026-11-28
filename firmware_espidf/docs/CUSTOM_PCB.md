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
| Microphones | ES7210 ADC (I2C 0x40, the same bus as touch: `lib/BoardI2c`), I2S 0 TDM RX at 16 kHz, MCLK 13, BCLK 12, WS 10, DIN 11. Slots: MIC1, MIC3 (the ES8311's output looped back, the echo reference), MIC2, MIC4 (not connected). Started on first use (`lib/VoiceCapture`); for now only the temporary `GET /rec/test.wav` uses it (see "Microphones: processing and tuning" below). The ES8311 (0x18) and the speaker amplifier (enable GPIO 53) are not driven |
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

## Microphones (esp-sr, in progress)

esp-sr 2.6.2 (`src/idf_component.yml`, P4 only) is in the build; `VoiceCapture` uses its GSC
beamformer and WebRTC NS (next section). A feasibility test (commit bcf8394, since removed) started the audio front end for
two mics ("MM": BSS and WebRTC noise suppression), then the GSC beamformer and WebRTC NS on their own, on
the panel. The AFE took 410 KB of PSRAM and about 10% of one core; GSC and WebRTC NS took 64.8 KB of
internal RAM and 42 KB of PSRAM. The plan is GSC plus WebRTC NS, without the AFE.

| | Flash (firmware.bin) | Static internal RAM |
|---|---|---|
| `custom_pcb` | 1,632,480 | 40.4 KB |
| + GSC + WebRTC NS | +61 KB | +1.1 KB |
| Phase 1b, as built (GSC + NS in `VoiceCapture`, the endpoint's knobs) | 1,695,376 -> 1,757,072 (+61,696) | +8 bytes |
| + the AFE (links every NS net, WakeNet, esp-dl) | +763 KB | +34 KB |

WebRTC NS, BSS and GSC need no models. Only NSNet (2-4), VADNet, WakeNet and MultiNet do: they live in
`srmodels.bin` in the `model` partition, which PlatformIO's build does not make or flash. To make one,
select the models in `sdkconfig.custom_pcb` (e.g. `CONFIG_SR_NSN_NSNET3=y` instead of
`CONFIG_SR_NSN_WEBRTC=y`; nsnet3 is 107 KB) and run esp-sr's packer, then flash it at the partition's offset:

```
python3 managed_components/espressif__esp-sr/model/movemodel.py -d1 sdkconfig.custom_pcb \
    -d2 managed_components/espressif__esp-sr -d3 .pio/build/custom_pcb
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32p4 -p <port> \
    -b 460800 --before no-reset write-flash 0xC20000 .pio/build/custom_pcb/srmodels/srmodels.bin
```

`esp_srmodel_init("model")` maps the whole partition and reads its first word as the model count, so
check for an erased partition (0xFFFFFFFF) before calling it: a panel updated only over the air has
no `model` partition at all, and one flashed without `srmodels.bin` has an erased one.

## Microphones: processing and tuning (phase 1b)

`VoiceCapture` runs the processing on its capture task as the audio comes in: MIC1 and MIC2 through
esp-sr's GSC beamformer, then WebRTC noise suppression (`ns_pro`), out as 16 kHz mono. A recording
holds any set of the stages' outputs (`VoiceCapture::Options::outputs`), interleaved, so the stages can
be compared on the same audio:

| Output | Channel |
|---|---|
| `OUT_MIC1`, `OUT_MIC2`, `OUT_REF` | The inputs as read (after the high-pass filter, if on) |
| `OUT_NS` | WebRTC NS on MIC1 alone |
| `OUT_BF` | The GSC beamformer on MIC1 + MIC2 |
| `OUT_BF_NS` | The beamformer, then WebRTC NS: the intended voice path |

The I2S reads are 40 ms (640 frames): five GSC frames (128 samples) and four NS frames (160, 10 ms), so
no stage buffers. esp-sr's handles are made at the start of each recording that needs them and freed at
its end (about 65 KB of internal RAM held only while recording; see `VoiceCapture::Pipeline`). Each
recording logs, at INFO (raise the manager's log level to see it):

```
VoiceCapture: Recorded 80000 frames (5000 ms) at 30 dB, outputs 0x39, NS level 1, HPF 0 Hz, beam 90 deg, spacing 65 mm:
VoiceCapture:   slot 0 MIC1       peak ..., RMS ... (-.. dBFS), mean ..      <- the inputs, before the HPF
VoiceCapture:   out NS(BF)   peak ... (-.. dBFS), RMS ... (-.. dBFS)         <- each output
VoiceCapture:   CPU: setup .. us; HPF .. us (..%), GSC .. us (..%), NS(MIC1) .. us (..%), NS(BF) .. us (..%); .. ms wall for 5000 ms of audio
VoiceCapture:   Heap: took .. bytes internal, .. PSRAM; free now ..; lowest since boot ..; stack .. bytes unused
```

The percentages are of one core, against the audio's length. "took" is this recording's esp-sr state
and chunk buffers (not the recording itself, which is in PSRAM: 32 KB a second a channel).

### Fetching clips (TEMPORARY endpoint)

`GET /rec/test.wav` records on request and answers with a WAV, 16 kHz 16-bit. It blocks for the
recording's length, then sends it; one recording at a time (503 otherwise). `<ip>` is the panel's.
The panel must run a build with this code: `pio run -e custom_pcb -t upload --upload-port <port>`
("Build and flash" below; over USB-OTG, unplug it afterwards to boot).

| Knob | Values (default) | What it does |
|---|---|---|
| `mode` | `raw` (default), `ns`, `bf`, `bf_ns`, `all` | `raw`: 3 channels MIC1, MIC2, REF, as before. `ns`, `bf`, `bf_ns`: mono, that stage's output. `all`: **4 channels from one recording**: 1 MIC1, 2 NS(MIC1), 3 BF, 4 NS(BF) |
| `secs` | 1-30 (5) | Length |
| `gain` | 0-37 dB (30) | The ES7210's analogue PGA gain, every mic. 36 dB peaks about -12 dBFS at speaking distance |
| `ns` | 0, 1, 2 (1) | WebRTC NS strength: mild, medium, aggressive. Applies to both NS outputs |
| `hpf` | 0, 1, or 20-2000 Hz (0) | Software 2nd-order Butterworth high-pass on the inputs, before everything (raw included). `1` = 100 Hz. For hum, rumble, HVAC |
| `angle` | 0-360 deg (90) | Beamformer steering, from the axis through MIC1 and MIC2. 90 = broadside, straight out of the panel: right for a talker in front |
| `spacing` | 5-500 mm (65) | MIC1-MIC2 distance for the beamformer's geometry. 65 is a placeholder: measure the board. At `angle=90` it matters little (broadside needs no steering delay) |
| `ch` | `all`, `1`, `2`, `ref` | `mode=raw` only: one input alone, mono |

```
P=<ip>
# The comparison: one recording, four stages, 4-channel WAV. Speak from 1-2 m, with some background noise.
curl -o all.wav "http://$P/rec/test.wav?mode=all&secs=8"
# Split it into one file per stage to listen to (sox):
for c in 1 2 3 4; do sox all.wav ch$c.wav remix $c; done   # 1 MIC1, 2 NS(MIC1), 3 BF, 4 NS(BF)
# Or with ffmpeg:
ffmpeg -i all.wav -filter_complex "channelsplit=channel_layout=4.0[a][b][c][d]" -map "[a]" mic1.wav -map "[b]" ns.wav -map "[c]" bf.wav -map "[d]" bf_ns.wav

# Sweeps, each on the comparison:
curl -o all-ns0.wav "http://$P/rec/test.wav?mode=all&secs=8&ns=0"
curl -o all-ns2.wav "http://$P/rec/test.wav?mode=all&secs=8&ns=2"
curl -o all-hpf.wav "http://$P/rec/test.wav?mode=all&secs=8&hpf=1"
curl -o all-g36.wav "http://$P/rec/test.wav?mode=all&secs=8&gain=36"
# The voice path alone (what phase 2 will send), and the raw inputs:
curl -o bf_ns.wav "http://$P/rec/test.wav?mode=bf_ns&secs=8"
curl -o raw.wav "http://$P/rec/test.wav?mode=raw&secs=8"
```

Different recordings are different audio; only `mode=all` compares stages on the same input. Across
knobs, repeat the same speech and noise. GSC and NS may delay their output by a few milliseconds
(esp-sr's own latency, not measured yet); align the channels before subtracting one from another. A
4-channel WAV may play as surround or only partly in some players: split it first. The INFO log lines
need the manager's log level at Info or Debug (or a first boot), read on the USB-UART port.

### The tuning pass

What to decide, in order, and how to tell:

1. **Gain.** Peaks around -6 to -12 dBFS on MIC1 when speaking at the expected distance, never 0 dBFS
   (`peak 32767` in the log is clipping). Too high clips loud talkers; too low loses the quiet ones and
   raises the noise floor relative to speech after NS.
2. **Does the beamformer help?** Compare channels 1 and 3 (MIC1 vs BF), and 2 and 4 (NS vs BF+NS), of
   one `mode=all` recording, with a noise source off to one side (a radio, a tap). With only two mics
   about 65 mm apart (correlation 0.88), the gain is small and mostly above 1-2 kHz; if BF+NS is not
   clearly better than NS alone, or sounds hollow or phasey, the plan can drop to NS on MIC1 (or on
   (MIC1+MIC2)/2) and save GSC's CPU and PSRAM.
3. **NS level.** `ns=0`, `1`, `2` on `mode=all`. Listen to NS(BF) for musical noise (warbling), clipped
   word starts and ends, and pumping. Pick the strongest level that keeps speech natural.
4. **HPF.** `hpf=1` (100 Hz), or 80-150. Keep it if the room has hum or rumble and the voice does not get
   thin. The ES7210 has its own DC filter, so it is off by default.
5. **Spacing and angle.** Only if the beamformer is kept: measure the mic spacing and set `spacing`;
   leave `angle=90` unless talkers are off to one side.
6. **Cost.** Read the `CPU:` and `Heap:` lines: GSC and NS should each be a few percent of a core; note
   the internal RAM "took" and the lowest free internal RAM, and report them.

Whatever is chosen becomes `Options`' defaults (`lib/VoiceCapture/VoiceCapture.hpp`), and phase 2's
streaming path records `MODE_BF_NS` (or the mode chosen) with them.

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
