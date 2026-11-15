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

CI runs the same script (`.github/workflows/qemu-tests.yml`). The suite takes about 15 minutes,
because every test boots a fresh panel. Per-test console and mosquitto logs land in
`qemu/build/test-logs/`, and CI uploads them as an artifact. So are the screenshots, in each
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
                         panel :80 forwarded to Rig.panel.web_port
```

| Piece | File | Role |
|---|---|---|
| `Rig` | `nspm_qemu/rig.py` | One panel and its world. `boot()`, `wake_to_home()`, `http_get()`. |
| `QemuPanel` | `nspm_qemu/panel.py` | Builds the flash and eFuse images, runs QEMU, gives `wait_for_log()`, detects panics. |
| `FakeNextion` | `nspm_qemu/fake_nextion.py` | The display emulator ([nextion-tools](https://github.com/jamesmulcahy/nextion-tools)' `nextion_tools.emu`, pinned in `requirements.txt`) as the NSPanel Manager TFT: models the screen from the panel's commands and injects user input. |
| `FakeManager` | `nspm_qemu/fake_manager.py` | Registration handshake, config, room and entity-page state, decoded commands. |
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
| `nspm_qemu/upstream.py`, `hmi.py`, `proto.py` | Library | Fetch and cache NSPanelManager's HMI and `.proto` files, and load them. |
| `nspm_qemu/gui.py`, `display_errors.py` | Data | Component names tests touch; display rejections known to be the firmware's. |
| `nspm_qemu/freertos_tasks.py` | gdb script | The task dump a failing test takes ([Debugging](#debugging)). |
| `tests/` | Tests | `conftest.py` has the fixtures; one file per area. |

The panel boots already configured. The harness builds the LittleFS partition from `data/` plus
a `config.json` that points MQTT at the broker. The eFuses carry a fixed MAC
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
- **Timing is not real.** QEMU runs the guest slower than real time and not cycle-accurately.
  Races may appear or vanish compared with hardware.
- **Buttons, relays and the buzzer** are GPIO/LEDC writes into models no test reads yet.

## Debugging

- `NSPM_QEMU_GDB=1` exposes a gdbstub on `:3333`. `NSPM_QEMU_GDB=wait` also halts at reset
  until GDB attaches:
  `~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32-elf-gdb .pio/build/qemu/firmware.elf -ex "target remote :3333"`.
  `thread apply all bt` shows both CPUs, which is how the ADC hang was found.
- **A failing test records what the guest was doing.** With `NSPM_QEMU_GDB` set (CI sets it), a test
  that fails while the guest is still running, which is how a hang or deadlock shows up, gets
  `cpus.txt` beside its console log, taken before the guest is stopped: both CPUs' backtraces,
  registers and running task, the kernel and flash locks, and every FreeRTOS task with its state,
  the queue, semaphore or mutex it waits on (and the mutex's holder), and the code addresses on
  its stack (`nspm_qemu/freertos_tasks.py`). A crash needs none of this: the panic is already in
  the console log. The task list needs a gdb with Python scripting, which needs Python 3.8–3.11
  from `uv` or on `PATH`.
- `NSPM_QEMU_SKIP_BUILD=1 qemu/run_tests.sh` reruns against the existing build.
- QEMU comes from `install_qemu.sh`, pinned by release and SHA-256, into `~/.cache/nspm-qemu`.
  Set `QEMU_XTENSA` to use another binary.

## The HMI and the protobufs

Both belong to NSPanelManager, which builds the TFTs and owns the schema; the firmware repo
carries neither (only protobuf-c output, in `lib/ProtoBuf`). The harness downloads
`docker/HMI_files/tft_automation/<region>/gui.HMI` (`nspm_qemu/hmi.py`) and
`docker/protobuf/*.proto` (`nspm_qemu/proto.py`, compiled with grpcio-tools) from the one commit
in **`nspanelmanager.ref`**, and caches them under `build/hmi/` and `build/proto/`. Bump that
line to move both; the file says what currently holds it back.

- `NSPM_NSPANELMANAGER_REF=<commit or branch>` uses another revision for one run.
- `NSPM_HMI=<path>` uses a local HMI, e.g. one not pushed yet. `NSPM_HMI_REGION=eu` tests the EU
  layout (default: US).
- `NSPM_PROTO_DIR=~/nspanel/NSPanelManager/docker/protobuf` uses a local checkout, to test a
  schema change before it is pushed.

The manager's schema can be newer than the one `lib/ProtoBuf` was generated from. That is
wire-compatible: the firmware ignores fields it does not know.
