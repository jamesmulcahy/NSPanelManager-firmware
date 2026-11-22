# QEMU firmware tests

These tests boot the real firmware image in
[Espressif's QEMU fork](https://github.com/espressif/qemu) and drive it as a panel would be
driven in a house. The firmware talks to a fake NSPanelManager through a real mosquitto, and a
fake Nextion display stands in for the screen, so tests can touch the screen and assert on what
the panel publishes and draws.

Needs `mosquitto` installed (`apt install mosquitto`). Each test starts its own instance on a
random port, so the system service can stay disabled.

```
firmware_espidf/qemu/run_tests.sh            # build, install QEMU if needed, run everything
firmware_espidf/qemu/run_tests.sh -k touch   # extra args go to pytest
```

That is the whole local command: nothing else needs setting. The display defaults to the LVGL UI
(`NSPM_QEMU_TERMINAL=us`), which is what the fleet runs, and the .proto files come from the pin in
`protobuf.ref`. The legacy Nextion TFT, which CI still covers, is a different leg:

```
NSPM_QEMU_TERMINAL=nextion firmware_espidf/qemu/run_tests.sh -k boot
```

Tests that need an environment the default local run should have (the LVGL UI, a schema with the
messages they use) abort the session at collection, listing the tests, before any panel boots;
`NSPM_QEMU_ALLOW_SKIPS=1` runs anyway and skips them. Skips that are expected locally (the
Nextion-only tests, the dev-stack tests without `NSPM_STACK_MQTT`) print a one-line summary at
collection. Tests that need a `-DNSPM_HEAP_DIAG` build skip at run time.

CI runs the same script (`.github/workflows/qemu-tests.yml`). The suite takes about 75 minutes,
because every test boots a fresh panel: about 15 for the core tests and 40 for the GUI update
tests (`-k "not gui_update"` skips them).
Per-test console and mosquitto logs land in `qemu/build/test-logs/`, and CI uploads them as an
artifact. So are the screenshots, in each
test's `screens/`: one PNG of every page the test waited for (`wait_for_page()`, taken as the page
appears, so often before the firmware has filled it in), and one of the screen when the test
ended (`NN-end.png`) or failed (`NN-failure.png`, also named on stderr). Brightness (`dim`) is not
applied: a screen the firmware has dimmed to 0 is drawn as if lit.

To use an emulated panel by hand instead, see [Using a panel from a browser](#using-a-panel-from-a-browser).

## Using a panel from a browser

```
firmware_espidf/qemu/web_panel.sh                       # prints the screen's URL; Ctrl-C stops it
firmware_espidf/qemu/web_panel.sh --rotation 90 --weather
```

This boots one panel against the fake manager, as a test would, and serves its screen with
nextion-tools' viewer at `http://127.0.0.1:8080/`. Click and drag on the screen to touch the
panel. The script prints each command the panel sends the manager. Options: `--http PORT`,
`--bind ADDR` (anyone who can reach it can touch the panel), `--rotation 0|90|180|270`, and
`--weather` for the weather screensaver with a forecast. It takes the same environment variables
as `run_tests.sh`, and keeps the console log, broker log and screenshots in
`qemu/build/web_panel/`. `tests/test_web_panel.py` checks it works: it wakes the panel and turns
on the lights, through the viewer alone.

## How it is wired

```
            QEMU -M esp32 (firmware built with [env:qemu])
   UART0 ──────────────► console: captured, crash-checked, saved per test
   UART2 ◄─────────────► FakeNextion       (TCP socket)
   open_eth ── slirp ──► host 10.0.2.2 ──► FaultProxy ──► mosquitto ◄── observer ◄──► FakeManager
                                       └──► UpdateServer (HTTP: firmware and TFT downloads and checksums)
                         panel :80 forwarded to Rig.panel.web_port
```

| Piece | File | Role |
|---|---|---|
| `Rig` | `nspm_qemu/rig.py` | One panel and its world. `boot()`, `wake_to_home()`, `http_get()`. |
| `QemuPanel` | `nspm_qemu/panel.py` | Builds the flash and eFuse images, runs QEMU, gives `wait_for_log()`, detects panics. |
| `FakeNextion` | `nspm_qemu/fake_nextion.py` | The display emulator ([nextion-tools](https://github.com/jamesmulcahy/nextion-tools)' `nextion_tools.emu`, pinned in `requirements.txt`) as the NSPanel Manager TFT: models the screen from the panel's commands and injects user input. |
| `FakeManager` | `nspm_qemu/fake_manager.py` | Registration handshake, config, room and entity-page state, decoded commands. |
| `UpdateServer` | `nspm_qemu/update_server.py` | The manager's HTTP side of an update: the firmware and the TFT (and a LittleFS image, which tests stage to prove the panel never fetches it) (with Django's Range quirks: 200 with `data[a:b]`, never 206). Records every request and injects faults at a file offset, aimed at one file with `target=`. |
| `Broker` | `nspm_qemu/broker.py` | A mosquitto per test. A paho client records all traffic, a TCP proxy in front of the panel lets tests cut its connection, and connects and subscriptions are read from mosquitto's log. |

### Files

| File | Kind | What it is for |
|---|---|---|
| `run_tests.sh` | Entry point | Builds the firmware, installs QEMU and the venv if needed, runs pytest. |
| `web_panel.sh`, `nspm_qemu/web_panel.py` | Entry point | One panel to use from a browser (above). |
| `env.sh` | Shell, sourced | What both entry points share: finds QEMU, mosquitto and PlatformIO, builds, sets up `.venv/`. |
| `install_qemu.sh` | Shell | Downloads Espressif's QEMU, pinned by release and SHA-256. |
| `gen_sdkconfig.py`, `sdkconfig.qemu.defaults` | Build | PlatformIO pre-script and overlay that make `sdkconfig.qemu` ([What differs from hardware](#what-differs-from-hardware)). |
| `nspanelmanager.ref` | Pin | The NSPanelManager commit the HMI and the `.proto` files come from ([The HMI and the protobufs](#the-hmi-and-the-protobufs)). |
| `requirements.txt` | Pin | Python packages, nextion-tools included. |
| `nspm_qemu/rig.py`, `panel.py`, `image.py`, `waiting.py` | Library | Running the panel: see the table above. `image.py` builds the flash and eFuse images. |
| `nspm_qemu/fake_manager.py`, `broker.py`, `fake_nextion.py` | Fake | The manager, broker and display the panel talks to. |
| `nspm_qemu/stack_manager.py`, `stack_web.py` | Library | The real manager on a dev stack, in place of the fake ([Stack tests](#stack-tests)): watches its MQTT, and accepts and deletes panels through its web API. |

| `nspm_qemu/fake_manager.py`, `broker.py`, `fake_nextion.py`, `update_server.py` | Fake | The manager, broker, display and update downloads the panel talks to. |
| `nspm_qemu/ota_update.py` | Library | Runs a firmware update on a `Rig` and reads LittleFS back from the flash image ([Web UI tests](#web-ui-tests)). |
| `nspm_qemu/gui_update.py` | Library | Runs a GUI (TFT) update on a `Rig` and checks the display's image against the served file ([GUI update tests](#gui-update-tests)). |
| `nspm_qemu/upstream.py`, `hmi.py`, `proto.py` | Library | Fetch and cache NSPanelManager's HMI and `.proto` files, and load them. |
| `nspm_qemu/gui.py`, `display_errors.py` | Data | Component names tests touch; display rejections known to be the firmware's. |
| `nspm_qemu/freertos_tasks.py` | gdb script | The task dump a failing test takes ([Debugging](#debugging)). |
| `tests/` | Tests | `conftest.py` has the fixtures; one file per area. |

The panel boots already configured. The harness builds the LittleFS partition from a
`config.json` that points MQTT at the broker, and nothing else: the setup web UI is in the app. The eFuses carry a fixed MAC
(`C0:49:EF:00:00:01`) with a valid CRC, so topics are stable.

## Writing a test

Fixtures (`tests/conftest.py`), each a fresh panel:

- `rig`: not started yet.
- `booted`: registered, configured, idle on the screensaver.
- `home`: woken to the home page.

```python
def test_ceiling_button_turns_on_ceiling_lights(home):
    mark = home.manager.command_mark()

    home.nextion.press(gui.HOME_CEILING)

    cmd = home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    assert cmd.first_page_turn_on.affect_lights == Cmd.CEILING_LIGHTS
```

**Simulating input** (`FakeNextion`). A touch is a finger on the screen, and the TFT's own code
(from `gui.HMI`) decides what reaches the firmware, as on a panel. Tests name components by their
`gui.HMI` names, kept once in `nspm_qemu/gui.py`, never by id: the display looks the name up in the
HMI, so an id that moves there is followed, and a name that disappears fails the touch.

| Call | What happens |
|---|---|
| `press(id, hold=0.1)` | A finger down on the component (by id or name, on the current page), then up. It reports the edges its `sendkey` asks for, and runs its own and the page's touch-capture code. On the items and lights pages that code decodes gestures, so `hold` must cover at least one 50 ms tick of the gesture timer. |
| `touch(id, pressed)` | Only the finger down, or only the lift. |
| `slide(name, value)` | A finger on the slider at the point for `value`; returns the value it took. |
| `swipe("down")`, `drag(start, end)`, `tap(x, y)`, `touch_down/move/up(x, y)` | Gestures and raw touches. |
| `wake()` | A tap in the middle of the screensaver, whose touch capture prints `0x93`. Touches nothing, and returns False, if no screensaver page is shown. |
| `sleep()` | The current page's `sleep_display` timer runs out now, and its code prints `0x92`. Left alone, it runs out after the screensaver timeout the firmware configures (`home.sleep_display.tim`). |
| `send_touch_event(id, pressed)`, `send_frame(code, ...)` | A bare frame, whatever the HMI would send. |
| `send_string(data)` | A `0x99` string event. |

**The display's speed.** By default the fake executes everything the moment it arrives. Set
`nextion.flow_model = True` to make it behave like the real display, buffer, stalls and lost
commands included, as measured on a real panel (see nextion-tools' README); `nextion.lost` and
`nextion.overflows` then say what did not fit. The firmware paces its writes to fit that buffer
(`lib/Nextion/NextionWindow`, which counts the display's `bkcmd=3` replies). `tests/test_weather.py`
runs with the flow model on, because booting to the weather screensaver and drawing its forecast
are the bursts that overflowed on panels before that pacing.

**The display knows the TFT.** The fake reads the TFT's Nextion project (`gui.HMI`), fetched from
NSPanelManager at the commit in `nspanelmanager.ref` (see
[The HMI and the protobufs](#the-hmi-and-the-protobufs)). It answers as a real display does: a page, component or attribute that does not
exist where it is used fails with `0x1A`; the label without its page prefix (`forDay1.txt`)
exists only while its page is shown; ids are the HMI's; `get` returns the HMI's value until one is
assigned; loading a page resets its local components.

**A rejected instruction fails the test.** On a panel, the display runs at `bkcmd=3`, so the
firmware hears each `0x1A`, but logs it only at DEBUG (`Display rejected '<instruction>': 0x1a`;
WARN for a bad picture id), and the label is still never drawn. That is easy to miss. Here every rejection not listed in
`nspm_qemu/display_errors.py` fails the test that caused it. List one there only with the reason,
and remove it when the firmware is fixed. Mark a test `@pytest.mark.display_errors_ok` if it
sends bad instructions on purpose.

**`tests/test_gui_data.py`** checks `lib/Pages/GUI_data.hpp` against the HMI without booting
anything: every page and component the firmware names exists, every id it listens for is a
component that reports touches (itself, or through the page's own `printh 65` code), and every
picture id is in range. Known mismatches are listed in the test, which also fails once one of
them is fixed, so the list stays honest.

**Looking at the screen:** `nextion.screenshot(name)` saves a PNG into the test's `screens/`, and
`nextion.snapshot()` gives the pixels as a numpy array. They draw from the HMI and what the
firmware assigned, as nextion-tools' renderer does (see its README for what is drawn).

**Observing the screen:** `page`, `page_history`, `components["page.comp.attr"]` (what the firmware
assigned), `value("page.comp.attr")` (what the display holds, HMI defaults included),
`system["dim"]`, `commands` (raw instructions), `errors` (rejected instructions), and the waits
`wait_for_page()`, `wait_for_value()`, `wait_for_command()` and `wait_for(predicate)`.

**Observing MQTT:** `broker.wait_for_message(topic_filter, predicate, since=broker.mark())`,
`broker.subscriptions(client)`, `broker.wait_for_subscription(...)`, and
`broker.drop_client(client)` to simulate a network fault. To publish manager-side state, change
`manager.rooms` / `manager.entity_pages` / `manager.config`, then call `publish_room()` /
`publish_entity_page()` / `publish_config()`.

Use the `mark()` / `since=` pairs. Without them, a wait can match something from boot.

A panic, `abort()`, failed assert or stack overflow on the console fails the test, even one that
did not wait long enough to see it. `PANIC_PRINT_HALT` is set, so the backtrace is the last thing
in the log. Decode it with `xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/qemu/firmware.elf <addrs>`.

**Which edges does a button send?** Whatever the HMI says: each component's `sendkey` (bit 0
release, bit 1 press; see `hmi_tool.py show FILE PAGE --attrs id,sendkey` in NSPanelManager), plus
any `printh 65 ...` its page's code sends for it. The scenes button reports release only, the
ceiling and table hotspots both, next-room and next-mode release only. `press()` sends exactly
that.

**The TFT's timers run in real time,** on the host's clock, not QEMU's. The one that matters is
`sleep_display`: after the firmware's screensaver timeout (FakeManager: 60 s) with no touch, the
page prints `0x92` and the panel goes to its screensaver, as a real one would.

## The LVGL UI (terminal TFT)

This is the default display.

```
firmware_espidf/qemu/run_tests.sh -k lvgl
firmware_espidf/qemu/web_panel.sh --terminal us
```

With `NSPM_QEMU_TERMINAL=us` (the default; or `eu-on-us`) the fake display runs
[nspanel-terminal](https://github.com/ptaru/nspanel-terminal)'s TFT instead of NSPM's: no HMI, no
`NSPM` banner, and nextion-tools' protocol-8 receiver behind it. The firmware's LvglUi then draws
every page and streams the pixels, and `FakeManager` says `is_us_panel` to match. Screenshots are
the receiver's framebuffer, turned upright; `nextion.tap_screen(x, y)` touches a point of the
screen as a screenshot shows it. `tests/test_lvgl_terminal.py`, `tests/test_timers.py` and the
screensaver card tests run only in this mode; their touch coordinates are for the US layout.
With `NSPM_QEMU_TERMINAL=nextion` the display is NSPM's Nextion TFT, the Nextion-page tests run
and the LVGL ones skip (expected there, so they do not abort the session).
Nextion support is going away, so only CI runs that leg regularly.

The receiver comes from nextion-tools 0.2.0 or later (pinned in `requirements.txt`).

Two things differ from a panel:

- **The receiver's 3 s watchdog is off (60 s).** It runs on the host's clock, and the boot-time
  LittleFS save stops the guest for ~20 s. The firmware's own reverse timeout, on the guest's
  clock, can still end the session then; it starts another, and `Rig.boot()` waits for it.
- **The stream is slow**, ~8 KB/s under QEMU against ~1.5 Mpx/s on a panel, so a full repaint
  takes seconds. Wait for the screen with `settle()` (in the test file), not fixed sleeps.

## Stack tests

`tests/test_stack.py` runs the firmware against the real NSPanelManager instead of
`FakeManager`, using the dev stack from the NSPanelManager repo (`docker/dev`). Start the
stack, then give the harness its broker:

```
docker/dev/nspm-dev up                                          # in NSPanelManager; prints the broker port
NSPM_STACK_MQTT=127.0.0.1:<port> qemu/run_tests.sh -m stack     # here
```

`NSPM_STACK_WEB` is the stack's web UI (default `http://127.0.0.1:8010`). Tests marked
`stack` are skipped without `NSPM_STACK_MQTT`.

```
   QEMU panel ── slirp ──► FaultProxy ──► stack mosquitto ◄──► MQTTManager ◄── Django
                                               ▲                    ▲ STOMP     ▲ REST
                                               └ observer ─── StackManager ─────┘
```

Each panel gets a random MAC and its own hostname, so it is new to the stack. It registers,
shows up as awaiting accept, and `StackManager.accept()` accepts it through the same STOMP
topics (`entity_states`, `nspanel/<mac>/status`) and REST call
(`POST /rest/nspanel/<id>/accept`) the web UI uses. That makes any number of panels
possible: `make_rig()` builds another one. At teardown the panel is deleted from the stack
(`NSPM_STACK_KEEP_PANELS=1` keeps it, to look at it in the UI).

The stack is shared state, so `subscriptions()` and the connect/subscribe waits, which read
the harness's own mosquitto log, are unavailable here.

CI runs these tests in `.github/workflows/stack-tests.yml`: it checks out NSPanelManager,
starts its dev stack, waits for MQTTManager to build, and runs `-m stack`. The NSPanelManager
repository and branch are set at the top of that workflow (and can be overridden when it is
run by hand).

## Web UI tests

The setup web UI (`web/index.html` and `web/static/main.css`) is gzipped by `gen_web_assets.py` and
built into the firmware, and the panel sends those bytes as they are, with
`Content-Encoding: gzip`. LittleFS holds only `/littlefs/config.json`. Every test's flash image is
built that way, so every boot test also proves the page does not come from the partition.

`tests/test_web_ui.py` checks:

- `/` and `/static/main.css` are sent gzipped, with the right `Content-Type`, and decompress to
  exactly the files in `web/`;
- the register request and the status report carry `"embedded"` as the web UI's checksum;
- a firmware update with a LittleFS image also staged on the manager never asks for it, leaves
  config.json as it was, and boots the new firmware (`run_update(rig)` sends `firmware_update` and
  waits for the panel to reboot and register again; it reads LittleFS back out of the flash file
  with littlefs-python, because PlatformIO's mklittlefs carries an older LittleFS that misses files
  the firmware wrote, config.json among them);
- a LittleFS partition that is all `0xFF` (`rig.panel.blank_littlefs = True`) is formatted on
  first mount, and the panel starts its setup network (Ethernet stands in for the AP under QEMU)
  and serves the page.

## GUI update tests

`tests/test_gui_update.py` runs a GUI (TFT) update on each upload protocol, and
`tests/test_gui_update_faults.py` repeats it with the network, the manager and the display
misbehaving. `rig.set_tft(data)` stages a TFT on `UpdateServer` and tells the display it is a
valid GUI. `run_gui_update(rig)` (`nspm_qemu/gui_update.py`) sends `tft_update`, waits for the
panel to reboot, and returns an `UpdateResult`: every upload the display saw, every download
request, what the firmware logged and the md5 it stored afterwards. `check_invariants(result)`
then checks that:

- I1: a stored md5 matches the display's image byte for byte;
- I2: "Nextion GUI update complete" is only logged for an image the manager served;
- I3: the update ends (reboot) instead of hanging;
- I4: every chunk is 4096 bytes except the last, and nothing is sent past the end.

Each case names the TFT the display starts with and the one the update installs: `hmi-to-hmi` and
`hmi-to-terminal` (the migration) need `NSPM_QEMU_TERMINAL=nextion`; `terminal-to-terminal` runs on
the default terminal display. `rig.set_tft(data, installs="terminal")` makes a completed upload of
that image turn the display into the terminal one at its next power cycle (`FakeNextion.installs`),
and `make_rig(display="hmi"|"terminal")` picks the TFT a panel's display starts with.

A failure says where the image goes wrong and where the wrong bytes came from in the file
(for example "chunk 14 of 49 holds the file's bytes from offset 53248", which is what the #45
bug looked like).

The display's side is nextion-tools' upload model: from `whmi-wri`/`whmi-wris` on it writes TFT
bytes at its own cursor, acks each 4096-byte chunk (`0x05`, or `0x08 + u32` after the first
chunk on v1.2), and shows SYSTEM DATA ERROR unless the image's md5 is in `valid_md5s`. It can
misbehave on request: `ack_delay`, `jump_offset`, `split_jump`, `reject_upload`, `drop_ack_at`,
`dup_ack_at` and `inject_at` fire once each, and `always_drop_ack_at` fires on every attempt.
`UpdateServer`'s offset faults (`reset_at`, `fin_at`, `stall_at`, `swap_file_at`) fire when a
ranged download reaches a file offset. Its request faults (`drop_next`, `respond_next`,
`ignore_range_next`, `no_content_length_next`) apply to the next requests, including the
panel's first GET, which has no Range and only reads the file's size.

The firmware power-cycles the display when an update starts (`Nextion::restart()`), and the fake
follows the `Power-cycling Nextion display` log line, since it cannot see the GPIO: an upload in
progress is lost, and the TFT announces itself again.

`run_gui_update` gives up after 180 s, or after 60 s in which the display sees no new bytes and
no new upload. The longest legitimate quiet spell is a 20 s ack timeout plus the 10 s display
restart. Tests for bugs that are not fixed yet are marked `xfail(strict=True)`, so a fix
shows up as a failure until its marker is removed.

## What differs from hardware

The `qemu` PlatformIO env is `original_sonoff` with `-DNSPM_QEMU` and
`sdkconfig.qemu.defaults` overlaid on `sdkconfig.original_sonoff`. `sdkconfig.qemu` is generated
(`gen_sdkconfig.py`). The overlay enables OpenCores Ethernet, sets 4 MB flash and halts on panic.
It also enlarges the Ethernet RX ring: ESP-IDF's OpenCores ISR logs dropped frames with format
strings in flash, which panics with a cache error if the ring overflows during a flash write.
Each `NSPM_QEMU` block in the source exists because QEMU does not emulate something:

| Where | Hardware | Under QEMU | Why |
|---|---|---|---|
| `WiFiManager::init` | `esp_wifi_init()`, Wi-Fi STA and AP netifs | Only `esp_netif_init()` and the MAC | No Wi-Fi radio in QEMU |
| `WiFiManager::start_client` | Wi-Fi STA, fallback AP after a timeout | OpenCores Ethernet + DHCP, reported through the same `connected()` / `ip_info()`; no fallback AP | No Wi-Fi radio in QEMU |
| `Nextion::_task_uart_event` | On `UART_PATTERN_DET`, reads everything buffered and splits it with `_post_received_frames()` | The same read and split, on `UART_DATA` | QEMU's UART has no AT_CMD_CHAR detection, so `UART_PATTERN_DET` never fires |
| `Nextion::init` | Enables pattern detection | Skipped | Not emulated. Also, QEMU's DPORT model doesn't reflect the UART clock-enable bits, so under IDF 6 `uart_get_baudrate()` fails and the setup divides by zero |
| `StatusUpdateManager::init` / `_measure_temperature` | NTC via ADC1 (`adc_oneshot` + calibration) | No ADC setup; fixed 1680 mV (≈22 °C) | The ADC is unimplemented; a conversion spins forever with a spinlock held and freezes core 0 |
| `StatusUpdateManager::_send_status_update` | `esp_wifi_sta_get_rssi()` | −50 | Wi-Fi is never initialised, and the call dereferences driver state |
| `WiFiManager::_apply_power_save` | `esp_wifi_set_ps()` for the page shown | Nothing | No Wi-Fi driver to set it on |

Consequences:

- **Wi-Fi reconnect logic is not exercised.** `WIFI_EVENT_STA_DISCONNECTED` is never posted, so
  hazards that involve Wi-Fi events on the default event loop cannot be reproduced here.
  MQTT-level faults can (`broker.drop_client`).
- **The Nextion receive path is triggered differently.** Frames are split by the same
  `_post_received_frames()` as on hardware, but on `UART_DATA` events rather than
  `UART_PATTERN_DET`. Bugs in the pattern-detect events themselves are out of reach.
- **A reset is a fresh QEMU.** QEMU runs with `-no-reboot`, so `esp_restart()` (after an update,
  for example) ends the process, and `QemuPanel` starts a new one on the same flash file: a clean
  power-on, because QEMU's in-place reset leaves stale interrupt state behind. The fake display
  stays up across it, and the new boot power-cycles it as on hardware (`Nextion::restart()`).
- **Timing is not real.** QEMU runs the guest slower than real time and not cycle-accurately.
  Races may appear or vanish compared with hardware.
- **Buttons, relays and the buzzer** are GPIO/LEDC writes into models no test reads yet.

## Debugging

- `NSPM_QEMU_GDB=1` exposes a gdbstub on a free port, logged as `gdbstub on :<port>`, so suites
  can run side by side. `run_tests.sh` sets it unless it is already set; `NSPM_QEMU_GDB=0` turns
  it off. `NSPM_QEMU_GDB=wait` uses `:3333` and halts at reset until GDB attaches:
  `~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32-elf-gdb .pio/build/qemu/firmware.elf -ex "target remote :3333"`.
  `thread apply all bt` shows both CPUs, which is how the ADC hang was found.
- **A failing test records what the guest was doing.** With `NSPM_QEMU_GDB` on (the default with `run_tests.sh`, and in CI), a test
  that fails while the guest is still running, which is how a hang or deadlock shows up, gets
  `cpus.txt` beside its console log, taken before the guest is stopped: both CPUs' backtraces,
  registers and running task, the kernel and flash locks, and every FreeRTOS task with its state,
  the queue, semaphore or mutex it waits on (and the mutex's holder), and the code addresses on
  its stack (`nspm_qemu/freertos_tasks.py`). A crash needs none of this: the panic is already in
  the console log. The task list needs a gdb with Python scripting, which needs Python 3.8–3.11
  from `uv` or on `PATH`.
- `NSPM_QEMU_SKIP_BUILD=1 qemu/run_tests.sh` reruns against the existing build.
  `NSPM_QEMU_BUILD_DIR=<dir>` runs against another build, for example a tree with a fix
  reverted to check that its test fails without it.
- QEMU comes from `install_qemu.sh`, pinned by release and SHA-256, into `~/.cache/nspm-qemu`.
  Set `QEMU_XTENSA` to use another binary.

## The HMI and the protobufs

Both belong to NSPanelManager, which builds the TFTs and owns the schema; the firmware repo
carries neither (only protobuf-c output, in `lib/ProtoBuf`). The harness downloads
`docker/HMI_files/tft_automation/<region>/gui.HMI` (`nspm_qemu/hmi.py`) from the upstream commit
in **`nspanelmanager.ref`**, and `docker/protobuf/*.proto` (`nspm_qemu/proto.py`, compiled with
grpcio-tools) from the `jamesmulcahy/NSPanelManager` commit in **`protobuf.ref`**, which has the
fork's schema additions. They are cached under `build/hmi/` and `build/proto/`; the files say what
holds each pin back, and bumping `protobuf.ref` takes a fork commit.

- `NSPM_NSPANELMANAGER_REF=<commit or branch>` uses another HMI revision for one run;
  `NSPM_PROTO_REF` does the same for the .proto files.
- `NSPM_HMI=<path>` uses a local HMI, e.g. one not pushed yet. `NSPM_HMI_REGION=eu` tests the EU
  layout (default: US).
- `NSPM_PROTO_DIR=~/nspanel/NSPanelManager/docker/protobuf` uses a local checkout, to test a
  schema change before it is pushed.

The manager's schema can be newer than the one `lib/ProtoBuf` was generated from. That is
wire-compatible: the firmware ignores fields it does not know.
