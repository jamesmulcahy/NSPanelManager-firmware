# Contributing

The firmware for NSPanels run by [NSPanel Manager](https://nspanelmanager.com). It is an
ESP-IDF project built with PlatformIO, in `firmware_espidf/`.

Open pull requests against `devel`.

## Building

From `firmware_espidf/`:

```
pio run -e original_sonoff                  # firmware for the Sonoff NSPanel (ESP32)
pio run -e custom_pcb                       # firmware for the custom PCB (ESP32-S3)
pio run -e original_sonoff -t buildfs       # the LittleFS image (the panel's web UI, from data/)
pio device monitor -e original_sonoff       # serial console, 115200 baud
```

The images land in `.pio/build/<env>/` (`firmware.bin`, `littlefs.bin`). `build_and_upload.sh`
builds both boards, bumps the version (unless `--no-ver`), and uploads the images to an NSPanel
Manager, which then offers them to its panels as an update.

## Testing

`firmware_espidf/qemu/` boots the real firmware in Espressif's QEMU, against a fake NSPanel
Manager, a real mosquitto and an emulated Nextion display running the NSPanel Manager TFT. Tests
touch the screen and check what the panel publishes and draws. You need `mosquitto` installed;
the script fetches QEMU and the Python packages itself.

```
firmware_espidf/qemu/run_tests.sh               # build, then run every test (about 15 minutes)
firmware_espidf/qemu/run_tests.sh -k touch      # extra arguments go to pytest
firmware_espidf/qemu/web_panel.sh               # one emulated panel, used from a browser
```

CI runs the same suite on every pull request that touches `firmware_espidf/`
(`.github/workflows/qemu-tests.yml`). [`firmware_espidf/qemu/README.md`](firmware_espidf/qemu/README.md)
covers writing a test, what the emulator does not cover, and debugging a failure.
