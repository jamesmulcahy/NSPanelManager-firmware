"""
Bring sdkconfig.custom_pcb (ESP32-P4) in line with sdkconfig.original_sonoff (ESP32).

Both boards build the same code, so the project's settings (LVGL widgets, no TLS stack, -Os, logging,
FREERTOS_USE_TRACE_FACILITY, stack sizes, ...) must match, while chip- and board-level settings stay
the P4's. Run it after changing sdkconfig.original_sonoff, then build custom_pcb once (ESP-IDF resolves
what depends on what was set) and commit both:

    python3 tools/sync_sdkconfig_custom_pcb.py sdkconfig.original_sonoff sdkconfig.custom_pcb sdkconfig.custom_pcb
    pio run -e custom_pcb

A second run right after the build changes nothing. To start over from the P4's defaults, delete
sdkconfig.custom_pcb, build custom_pcb once (it fails to link, but writes the defaults), copy that file
aside and pass it as the second argument.

An option whose value differs is taken from original_sonoff unless KEEP_P4 matches it (that is also where the P4-only
overrides live: the TCP send/receive buffers and the receive mailbox, see the comment there). With
esp_wifi_remote (Wi-Fi over ESP-Hosted), ESP_WIFI_<X> is a prompt-less mirror of WIFI_RMT_<X>, so
original_sonoff's ESP_WIFI_<X> is set as WIFI_RMT_<X>.
"""
import re, sys
if len(sys.argv) != 4:
    sys.exit(__doc__)
sonoff_path, p4_path, out_path = sys.argv[1:4]
LINE = re.compile(r'^(?:CONFIG_(\w+)=(.*)|# CONFIG_(\w+) is not set)$')
def parse(p):
    d = {}
    for l in open(p):
        m = LINE.match(l.strip())
        if m: d[m[1] or m[3]] = m[2] if m[1] else 'n'
    return d
sonoff, p4 = parse(sonoff_path), parse(p4_path)
# Chip- and board-level: the P4's own defaults stay.
KEEP_P4 = re.compile(r'^(SOC_|IDF_|BOOTLOADER_CPU_CLK|BOOTLOADER_OFFSET|BROWNOUT|ESP_BROWNOUT|COMPILER_FLOAT_LIB|EFUSE_|ESP_EFUSE_|'
                     r'ESP32_WIFI_|ESP_WIFI_REMOTE_ENABLED|ESPTOOLPY_FLASH|FLASHMODE_|ESP_DEFAULT_CPU_FREQ|ESP_MAC_UNIVERSAL|ESP_REV_|ESP_ROM_|'
                     r'ESP_SLEEP_|PM_|SD_ENABLE_SDIO|SPI_FLASH_SUPPORT_|ULP_|SPIRAM|PARTITION_|'
                     # TCP buffers (and their pre-rename names): the P4 serves 480 KB recordings to Home Assistant over
                     # Wi-Fi via the C6 and has the RAM; the Sonoff's 4 x MSS windows stay as they are.
                     r'(LWIP_)?TCP_(SND_BUF_DEFAULT|WND_DEFAULT|RECVMBOX_SIZE)$)')
want = {}
for k in sorted(set(sonoff) & set(p4)):
    if KEEP_P4.match(k):
        continue
    if k.startswith('ESP_WIFI_'):
        # With esp_wifi_remote (ESP-Hosted), ESP_WIFI_<X> is a prompt-less mirror of WIFI_RMT_<X>: set that.
        rmt = 'WIFI_RMT_' + k[len('ESP_WIFI_'):]
        if rmt in p4 and sonoff[k] != p4[rmt]:
            want[rmt] = sonoff[k]
        continue
    if sonoff[k] != p4[k]:
        want[k] = sonoff[k]
board = {
    # 32 MB QIO flash, the P4's own partition table.
    # (The string forms, ESPTOOLPY_FLASHMODE, ESPTOOLPY_FLASHSIZE and PARTITION_TABLE_FILENAME, are derived.)
    'ESPTOOLPY_FLASHMODE_QIO': 'y', 'ESPTOOLPY_FLASHMODE_DIO': 'n',
    'ESPTOOLPY_FLASHSIZE_32MB': 'y', 'ESPTOOLPY_FLASHSIZE_2MB': 'n',
    'PARTITION_TABLE_CUSTOM': 'y', 'PARTITION_TABLE_SINGLE_APP': 'n',
    'PARTITION_TABLE_CUSTOM_FILENAME': '"partitions_p4.csv"',
    # 32 MB PSRAM in the package.
    'SPIRAM': 'y',
    # The C6 on the SDIO pins of Espressif's P4 function EV board, as Waveshare's examples set.
    'ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD': 'y', 'ESP_HOSTED_P4_DEV_BOARD_NONE': 'n',
    # The P4's bootloader starts at 0x2000, so only 0x6000 bytes fit before the partition table (0x8000);
    # built for speed, as the Sonoff's is (0x1000, 0x7000 bytes of room), it is ~25 KB and overlaps it.
    'BOOTLOADER_COMPILER_OPTIMIZATION_SIZE': 'y', 'BOOTLOADER_COMPILER_OPTIMIZATION_PERF': 'n',
    # No Ethernet on this board.
    'ETH_USE_ESP32_EMAC': 'n', 'ETH_USE_SPI_ETHERNET': 'n',
}
want.update(board)
out, done, prev_default = [], set(), False
for l in open(p4_path):
    m = LINE.match(l.strip())
    key = m and (m[1] or m[3])
    if key in want:
        if out and out[-1].strip() == '# default:':
            out.pop()  # set on purpose: not a default kconfgen may recompute
        v = want[key]
        out.append(f'# CONFIG_{key} is not set\n' if v == 'n' else f'CONFIG_{key}={v}\n')
        done.add(key)
    else:
        out.append(l)
missing = sorted(set(want) - done)
for key in missing:  # symbols the defaults do not list yet (depend on one set above)
    v = want[key]
    out.append(f'# CONFIG_{key} is not set\n' if v == 'n' else f'CONFIG_{key}={v}\n')
open(out_path, 'w').writelines(out)
print(len(want), 'set;', 'appended:', missing)
