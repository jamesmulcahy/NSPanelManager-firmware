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

`VoiceCapture` runs the processing on its capture task as the audio comes in. A recording holds any
set of the stages' outputs (`VoiceCapture::Options::outputs`), interleaved, so the stages can be
compared on the same audio:

| Output | Channel |
|---|---|
| `OUT_MIC1`, `OUT_MIC2`, `OUT_REF` | The inputs as read (after the high-pass filter, if on) |
| `OUT_NS` | WebRTC NS on MIC1 alone |
| `OUT_BF` | The GSC beamformer on MIC1 + MIC2 (kept, not on the voice path) |
| `OUT_BF_NS` | The beamformer, then WebRTC NS (kept, not on the voice path) |
| `OUT_VOICE` | **The voice chain**: mic select, HPF, NS, makeup gain, limiter, mono |
| `OUT_VOICE_B` | The same chain on `voice_mic_b`: to compare mic choices in one recording |

### The first tuning round, and what it decided

Two 8 s recordings on hardware, ES7210 gain 36 dB, MIC spacing 60 mm:

| Output | Speech (95th-pct 20 ms RMS) | Noise floor | SNR | Energy < 300 Hz |
|---|---|---|---|---|
| MIC1 raw, HPF on | -38.8 dBFS | -66.7 | 27.9 dB | 36% |
| NS(MIC1), HPF on | -39.1 | -78.8 | 39.7 dB | 33% |
| BF, HPF on | -41.4 | -70.2 | 28.8 dB | 46% |
| BF+NS, HPF on | -42.6 | -83.5 | 40.9 dB | 42% |

- **The beamformer is off the voice path.** BF+NS has the best number (SNR 40.9 vs 39.7 dB) but it is
  1 dB, and by ear NS(MIC1) was the clearest. GSC costs 13.4% of a core, makes speech quieter (-2.3 dB)
  and adds low end (46% below 300 Hz against 36%): with two microphones 60 mm apart there is little
  for it to steer. Its code stays, and `mode=bf` / `bf_ns` / `stages` still reach it.
- **HPF at 100 Hz stays on** (0.54% of a core). Without it raw was 58% below 300 Hz and the floor 4 dB higher.
- **Playback is too quiet**: speech peaks only about -24 dBFS even at 36 dB analogue gain (the ES7210's
  ceiling is 37.5 dB). Hence digital makeup gain, below.
- MIC2 reads 1-3 dB hotter than MIC1 (peak 3880 vs 2818, RMS 254 vs 228).
- The capture task's stack used about 1.8 KB of 8 KB even with GSC: cut to 4 KB.
- Heap: two NS instances took 77.6 KB internal; the lowest free internal heap since boot was 238 KB.
- Outputs lag MIC1: NS 6 ms, BF 4 ms, BF+NS 10 ms (align before subtracting).

### The voice chain

`VoiceCapture::MODE_VOICE` (= `OUT_VOICE`), recorded with `VoiceCapture::voice_options()`:

```
mic (MIC1 | MIC2 | (MIC1+MIC2)/2) -> HPF 100 Hz -> WebRTC NS level 1 -> makeup (AGC) -> limiter -> mono int16
```

Defaults of `voice_options()`: ES7210 gain 36 dB, HPF 100 Hz, NS level 1, mic = **average**, AGC on
(target -20 dBFS, max +24 dB, idle +12 dB), limiter ceiling -3 dBFS.

- **Mic: the average.** The two microphones' uncorrelated self-noise adds in power, the speech from the
  front adds in amplitude, so averaging is about 3 dB better in SNR than either alone, and it evens out
  MIC2's 1-3 dB higher level. It costs 0.1% of a core. Whether it also sounds better than MIC1 or MIC2
  alone is what `mode=cmp` is for; if an off-axis talker or a comb-filter colour is audible, `mic=2` or
  `mic=1` is one knob.
- **Makeup: an AGC with a gain ceiling, not a fixed gain.** A fixed gain that puts speech at -3 dBFS
  at 1 m clips at 0.5 m and is quiet at 3 m; speech here peaks about -24 dBFS, so it needs about +20 dB,
  and a limiter alone would be doing the levelling. esp-sr's own AGC was not used: it is the WebRTC AGC,
  and the part that matters here is the gate, which only this code has (below).
  - Per 10 ms frame (one NS frame) the AGC measures the RMS after NS. Frames above -60 dBFS are speech:
    a level envelope follows them (up fast, down over about a second), and the gain heads for
    `target - envelope`, clamped to 0..+24 dB: down fast (about 30 ms, a loud talker), up slowly (about
    0.5 s, so it does not pump between words).
  - Frames below the gate (NS leaves about -79 dBFS of room) never raise the gain; they let it drift
    to the idle gain (12 dB), so the floor ends at about -67 dBFS, not -55, and a pause does not leave
    +24 dB waiting for the next breath of noise.
  - The target is the speech *envelope's* 10 ms RMS: speech has about 15 dB of crest (95th-pct RMS
    -39 dBFS, peaks -24), so -20 dBFS puts peaks at about -5 dBFS.
  - The gain is ramped across each 10 ms frame, and the state is carried from chunk to chunk, so it
    works the same on the streaming path's 320-frame halves as on the test endpoint.
- **Limiter**: a peak limiter after the gain, ceiling -3 dBFS. Per sample, the gain drops at once to
  whatever keeps the sample at the ceiling (attack 1 sample, so it cannot overshoot), and recovers
  over 80 ms. Nothing passes -3 dBFS, so no wraparound, no hard clip. With the AGC on, it should
  rarely act (see the log).

Each recording's log lists, per voice chain, the makeup gain actually applied (min / avg / max dB) and
how much the limiter did (frames and samples limited):

```
VoiceCapture:   VOICE (mic avg, AGC, target -20 dBFS): makeup gain min 12.0 / avg 19.4 / max 24.0 dB; limiter on 3 of 800 frames (41 samples, 0.03%)
```

### Fetching clips (TEMPORARY endpoint)

`GET /rec/test.wav` records on request and answers with a WAV, 16 kHz 16-bit. It blocks for the
recording's length, then sends it; one recording at a time (503 otherwise). `<ip>` is the panel's.
The panel must run a build with this code: `pio run -e custom_pcb -t upload --upload-port <port>`
("Build and flash" below; over USB-OTG, unplug it afterwards to boot).

| Knob | Values (default) | What it does |
|---|---|---|
| `mode` | `raw` (default), `ns`, `bf`, `bf_ns`, `stages`, `voice`, `cmp` (= `all`) | `raw`: 3 channels MIC1, MIC2, REF. `ns`, `bf`, `bf_ns`, `voice`: mono, that output. `cmp`: **4 channels from one recording**: 1 MIC1 raw, 2 NS(MIC1), 3 voice on MIC1, 4 voice on `mic=` (avg). `stages`: the old comparison: MIC1, NS(MIC1), BF, NS(BF). `voice` and `cmp` start from the production options (gain 36, hpf 100): other knobs override |
| `secs` | 1-30 (5) | Length |
| `gain` | 0-37 dB (30; 36 for voice, cmp) | The ES7210's analogue PGA gain, every mic |
| `ns` | 0, 1, 2 (1) | WebRTC NS strength: mild, medium, aggressive |
| `hpf` | 0, 1, or 20-2000 Hz (0; 100 for voice, cmp) | Software 2nd-order Butterworth high-pass on the inputs. `1` = 100 Hz |
| `mic` | `1`, `2`, `avg` (avg) | `voice`: the chain's input. `cmp`: channel 4's input (channel 3 is always MIC1) |
| `makeup` | -20 to 40 dB (12) | With `agc=0`, the fixed makeup gain; with the AGC, the gain it starts at and settles to in silence |
| `agc` | 0, 1 (1) | AGC on, or a fixed `makeup` gain |
| `target` | -40 to -3 dBFS (-20) | The AGC's target speech level (10 ms RMS of the envelope) |
| `maxgain` | 0-40 dB (24) | The AGC's gain ceiling |
| `angle`, `spacing` | 0-360 deg (90), 5-500 mm (60) | The beamformer (`bf`, `bf_ns`, `stages` only) |
| `ch` | `all`, `1`, `2`, `ref` | `mode=raw` only: one input alone, mono |

### Next listening round

```
P=<ip>
# 1. Mic choice and loudness on identical input. Speak from 1-2 m, with some background noise.
#    One recording, 4 channels: 1 MIC1 raw, 2 NS(MIC1), 3 voice on MIC1, 4 voice on the average.
curl -o cmp.wav "http://$P/rec/test.wav?mode=cmp&secs=10"
for c in 1 2 3 4; do sox cmp.wav cmp$c.wav remix $c; done
# Compare 1 vs 2 (NS alone), 2 vs 3 (makeup + limiter: louder, same clarity?), 3 vs 4 (MIC1 vs the average).
# The same, against MIC2 in channel 4:
curl -o cmp-mic2.wav "http://$P/rec/test.wav?mode=cmp&secs=10&mic=2"

# 2. Loudness: AGC target, fixed gain, and the soft/loud/far talker (repeat the same speech):
curl -o v-t16.wav "http://$P/rec/test.wav?mode=voice&secs=10&target=-16"
curl -o v-t24.wav "http://$P/rec/test.wav?mode=voice&secs=10&target=-24"
curl -o v-fixed20.wav "http://$P/rec/test.wav?mode=voice&secs=10&agc=0&makeup=20"
curl -o v-max30.wav "http://$P/rec/test.wav?mode=voice&secs=10&maxgain=30"

# 3. The production chain as the recorder will take it, and the old beamformer for reference:
curl -o voice.wav "http://$P/rec/test.wav?mode=voice&secs=10"
curl -o stages.wav "http://$P/rec/test.wav?mode=stages&secs=10&gain=36&hpf=1"
```

What to listen and look for: is the voice loud enough on the speaker at about -5 dBFS peaks; is the
room noise between words audible after the makeup (the AGC's idle gain, `makeup=`, lower it if so);
pumping or breathing when you pause (AGC release); crackle or flattening on loud syllables (the log's
limiter percentage; above a few percent, lower `target` or `maxgain`). In the log, per voice chain:
makeup gain min/avg/max, limiter frames, and CPU. A 4-channel WAV may play as surround or only
partly in some players: split it first. The INFO log lines need the manager's log level at Info or
Debug, read on the USB-UART port.

Different recordings are different audio; only `mode=cmp` and `mode=stages` compare on the same input.

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
