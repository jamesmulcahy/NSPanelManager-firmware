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

CI runs the same script (`.github/workflows/qemu-tests.yml`). The suite takes about 6 minutes,
because every test boots a fresh panel. Per-test console and mosquitto logs land in
`qemu/build/test-logs/`, and CI uploads them as an artifact.

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
| `FakeNextion` | `nspm_qemu/fake_nextion.py` | Models the screen from the panel's commands and injects user input. |
| `FakeManager` | `nspm_qemu/fake_manager.py` | Registration handshake, config, room and entity-page state, decoded commands. |
| `Broker` | `nspm_qemu/broker.py` | A mosquitto per test. A paho client records all traffic, a TCP proxy in front of the panel lets tests cut its connection, and connects and subscriptions are read from mosquitto's log. |

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

    home.nextion.press(22)  # GUI_HOME_PAGE::button_ceiling_id

    cmd = home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    assert cmd.first_page_turn_on.affect_lights == Cmd.CEILING_LIGHTS
```

**Simulating input** (`FakeNextion`):

| Call | What the display sends |
|---|---|
| `touch(id, pressed)` | One `0x65` touch event, either the press edge or the release edge. |
| `press(id, hold=0.1)` | Press, then release. Some handlers act on both edges (see below). |
| `slide(name, id, value)` | Sets what `get <name>.val` returns, then sends the release event. |
| `wake()` / `sleep()` | `0x93` / `0x92`. `wake()` is "someone touched the screensaver". |
| `send_string(data)` | A `0x99` string event. |

**Observing the screen:** `page`, `page_history`, `components["page.comp.attr"]`, `system["dim"]`,
`commands` (raw instructions), and the waits `wait_for_page()`, `wait_for_value()`,
`wait_for_command()` and `wait_for(predicate)`.

**Observing MQTT:** `broker.wait_for_message(topic_filter, predicate, since=broker.mark())`,
`broker.subscriptions(client)`, `broker.wait_for_subscription(...)`, and
`broker.drop_client(client)` to simulate a network fault. To publish manager-side state, change
`manager.rooms` / `manager.entity_pages` / `manager.config`, then call `publish_room()` /
`publish_entity_page()` / `publish_config()`.

Use the `mark()` / `since=` pairs. Without them, a wait can match something from boot.

A panic, `abort()`, failed assert or stack overflow on the console fails the test, even one that
did not wait long enough to see it. `PANIC_PRINT_HALT` is set, so the backtrace is the last thing
in the log. Decode it with `xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/qemu/firmware.elf <addrs>`.

**Which edges does a button send?** On the real TFT, each component is configured to report its
press event, its release event, or both. The firmware's handlers reveal the choice: the scenes
button checks `!pressed`, the ceiling and table buttons handle both, and next-room and next-mode
act on any event, which means the TFT reports one edge for them. Send what the TFT sends. A full
`press()` on next-room advances two rooms.

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
| `Nextion::_task_uart_event` | UART pattern-detect interrupt splits frames on `FF FF FF` | Software framer on `UART_DATA` (`_qemu_frame_uart_data`) | QEMU's UART has no AT_CMD_CHAR detection, so the display's replies would be dropped |
| `Nextion::init` | Enables pattern detection | Skipped | Not emulated. Also, QEMU's DPORT model doesn't reflect the UART clock-enable bits, so under IDF 6 `uart_get_baudrate()` fails and the setup divides by zero |
| `StatusUpdateManager::init` / `_measure_temperature` | NTC via ADC1 (`adc_oneshot` + calibration) | No ADC setup; fixed 1680 mV (≈22 °C) | The ADC is unimplemented; a conversion spins forever with a spinlock held and freezes core 0 |
| `StatusUpdateManager::_send_status_update` | `esp_wifi_sta_get_rssi()` | −50 | Wi-Fi is never initialised, and the call dereferences driver state |

Consequences:

- **Wi-Fi reconnect logic is not exercised.** `WIFI_EVENT_STA_DISCONNECTED` is never posted, so
  the event-queue starvation hazards in CLAUDE.md involving Wi-Fi cannot be reproduced here.
  MQTT-level faults can (`broker.drop_client`).
- **The Nextion receive path is not the production one.** Frame splitting, and the order and
  timing of `UART_DATA` versus `UART_PATTERN_DET` events, differ. Bugs in that handling are
  out of reach.
- **Timing is not real.** QEMU runs the guest slower than real time and not cycle-accurately.
  Races may appear or vanish compared with hardware.
- **Buttons, relays and the buzzer** are GPIO/LEDC writes into models no test reads yet.

## Debugging

- `NSPM_QEMU_GDB=1` exposes a gdbstub on `:3333`. `NSPM_QEMU_GDB=wait` also halts at reset
  until GDB attaches:
  `~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32-elf-gdb .pio/build/qemu/firmware.elf -ex "target remote :3333"`.
  `thread apply all bt` shows both CPUs, which is how the ADC hang was found.
- `NSPM_QEMU_SKIP_BUILD=1 qemu/run_tests.sh` reruns against the existing build.
- QEMU comes from `install_qemu.sh`, pinned by release and SHA-256, into `~/.cache/nspm-qemu`.
  Set `QEMU_XTENSA` to use another binary.

## Protobufs

The firmware repo carries only protobuf-c output (`lib/ProtoBuf`), so `nspm_qemu/proto.py`
fetches the `.proto` sources from NSPanelManager, which owns the schema. It downloads
`docker/protobuf/*.proto` at a pinned commit (`PROTO_REF`), caches them under `build/proto/`,
and compiles them with grpcio-tools.

- `NSPM_PROTO_REF=<commit or branch>` uses another revision. Bump `PROTO_REF` to move the
  default.
- `NSPM_PROTO_DIR=~/nspanel/NSPanelManager/docker/protobuf` uses a local checkout, to test a
  schema change before it is pushed.

The manager's schema can be newer than the one `lib/ProtoBuf` was generated from. That is
wire-compatible: the firmware ignores fields it does not know.
