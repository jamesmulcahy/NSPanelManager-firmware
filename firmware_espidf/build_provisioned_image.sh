#!/bin/bash
# Build a merged flash image with the Wi-Fi and MQTT settings already in
# /littlefs/config.json, so a freshly flashed panel joins Wi-Fi and the
# manager's MQTT broker on first boot instead of starting its setup AP.
#
# Settings are taken, in order of preference, from provision.env (next to this
# script), from the environment, from the manager and from a working panel. The
# manager supplies mqtt_server/port/username, and a panel supplies wifi_ssid.
# Neither ever returns a password, so WIFI_PSK and MQTT_PASSWORD are prompted
# for unless set. Copy provision.env.example to provision.env to skip prompts.
#
# The panel keeps its default name (NSPMPanel-<last 3 MAC bytes>), which is
# unique per device; rename it in the manager once it has registered.
#
# The output holds both passwords in plain text. Never upload it to the manager
# (build_and_upload.sh, save_new_merged_flash): the manager serves its images
# to anyone who asks.
set -euo pipefail

BOARD_TYPE="${1:-}"
cd "$(dirname "$0")"

if [ "${BOARD_TYPE}" == "custom" ]; then
  PIO_ENV="custom_pcb"
  CHIP_ARGS=(--chip esp32s3 merge_bin --flash_mode qio --flash_size 16MB)
  # The ESP32-S3 boots its second-stage bootloader from 0x0 (CONFIG_BOOTLOADER_OFFSET_IN_FLASH in
  # sdkconfig.custom_pcb); the ESP32 from 0x1000.
  BOOTLOADER_OFFSET=0x0
elif [ "${BOARD_TYPE}" == "sonoff" ]; then
  PIO_ENV="original_sonoff"
  CHIP_ARGS=(--chip esp32 merge_bin --flash_mode dio --flash_size 4MB)
  BOOTLOADER_OFFSET=0x1000
else
  echo "Syntax: $0 <sonoff|custom> [output file]"
  exit 1
fi
OUTPUT="${2:-provisioned-flash-${BOARD_TYPE}.bin}"
BUILD_DIR=".pio/build/${PIO_ENV}"

if [ -f provision.env ]; then
  # shellcheck disable=SC1091
  . ./provision.env
fi

function get_partition_field {
  grep -Eve "^#" partitions.csv | grep "$1," | awk -v field="$2" '{ print $field }' | cut -d ',' -f 1
}

# Prefer tools on PATH, else the copies PlatformIO installs.
PIO_PACKAGES="${HOME}/.platformio/packages"
MKLITTLEFS="$(command -v mklittlefs || echo "${PIO_PACKAGES}/tool-mklittlefs/mklittlefs")"
if command -v esptool.py >/dev/null; then
  ESPTOOL_CMD=(esptool.py)
elif [ -f "${PIO_PACKAGES}/tool-esptoolpy/esptool.py" ]; then
  ESPTOOL_CMD=("${HOME}/.platformio/penv/bin/python" "${PIO_PACKAGES}/tool-esptoolpy/esptool.py")
else
  ESPTOOL_CMD=()
fi
if [ ! -x "${MKLITTLEFS}" ] || [ ${#ESPTOOL_CMD[@]} -eq 0 ]; then
  echo "Need mklittlefs and esptool.py. Both come with PlatformIO; run a build once to install them."
  exit 2
fi

for bin in bootloader.bin partitions.bin firmware.bin; do
  if [ ! -f "${BUILD_DIR}/${bin}" ]; then
    echo "Missing ${BUILD_DIR}/${bin}. Build first: pio run --environment ${PIO_ENV}"
    exit 2
  fi
done

# Read the manager's MQTT settings. Only fill in what provision.env/the
# environment left unset, so either can override the manager.
if [ -n "${NSPM_MANAGER:-}" ] && { [ -z "${MQTT_SERVER:-}" ] || [ -z "${MQTT_PORT:-}" ] || [ -z "${MQTT_USERNAME+set}" ]; }; then
  echo "Reading MQTT settings from ${NSPM_MANAGER}"
  manager_settings="$(curl -fsS "${NSPM_MANAGER%/}/rest/settings")"
  read_setting() {
    python3 -c 'import json, sys; print(json.load(sys.stdin)["settings"].get(sys.argv[1], ""))' "$1" <<<"${manager_settings}"
  }
  MQTT_SERVER="${MQTT_SERVER:-$(read_setting mqtt_server)}"
  MQTT_PORT="${MQTT_PORT:-$(read_setting mqtt_port)}"
  MQTT_USERNAME="${MQTT_USERNAME-$(read_setting mqtt_username)}"
fi

# Read the SSID from a panel that is already on the network.
if [ -z "${WIFI_SSID:-}" ] && [ -n "${NSPM_PANEL:-}" ]; then
  echo "Reading Wi-Fi SSID from panel ${NSPM_PANEL}"
  WIFI_SSID="$(curl -fsS "http://${NSPM_PANEL}/config_data" | python3 -c 'import json, sys; print(json.load(sys.stdin)["wifi_ssid"])')"
fi

function prompt {
  local var="$1" text="$2" default="${3:-}" secret="${4:-}" value
  if [ -n "${secret}" ]; then
    read -r -s -p "${text}: " value
    echo
  elif [ -n "${default}" ]; then
    read -r -p "${text} [${default}]: " value
    value="${value:-${default}}"
  else
    read -r -p "${text}: " value
  fi
  printf -v "${var}" '%s' "${value}"
}

if [ -z "${WIFI_SSID:-}" ]; then
  current_ssid="$(nmcli -t -f active,ssid dev wifi 2>/dev/null | sed -n 's/^yes://p' | head -n 1 || true)"
  prompt WIFI_SSID "Wi-Fi SSID" "${current_ssid}"
fi
[ -n "${WIFI_PSK+set}" ] || prompt WIFI_PSK "Wi-Fi password for ${WIFI_SSID}" "" secret
[ -n "${MQTT_SERVER:-}" ] || prompt MQTT_SERVER "MQTT server"
[ -n "${MQTT_PORT:-}" ] || prompt MQTT_PORT "MQTT port" 1883
[ -n "${MQTT_USERNAME+set}" ] || prompt MQTT_USERNAME "MQTT username (blank for none)"
[ -n "${MQTT_PASSWORD+set}" ] || prompt MQTT_PASSWORD "MQTT password for ${MQTT_USERNAME:-anonymous} (blank for none)" "" secret

if [ -z "${WIFI_SSID}" ] || [ -z "${MQTT_SERVER}" ]; then
  echo "A Wi-Fi SSID and an MQTT server are required."
  exit 1
fi
case "${MQTT_SERVER}" in
localhost | 127.* | ::1)
  echo "MQTT server '${MQTT_SERVER}' is a loopback address the panel cannot reach. Set MQTT_SERVER in provision.env."
  exit 1
  ;;
esac

# Everything below handles the passwords, so keep it out of reach of other users.
umask 077
STAGING="$(mktemp -d)"
trap 'rm -rf "${STAGING}"' EXIT

mkdir "${STAGING}/fs"
# Passed through the environment rather than argv so the passwords never appear
# in the process list. Keys not written here keep ConfigManager's defaults.
WIFI_SSID="${WIFI_SSID}" WIFI_PSK="${WIFI_PSK}" MQTT_SERVER="${MQTT_SERVER}" MQTT_PORT="${MQTT_PORT}" \
  MQTT_USERNAME="${MQTT_USERNAME}" MQTT_PASSWORD="${MQTT_PASSWORD}" \
  python3 -c '
import json, os, sys
config = {
    "log_level": 2,  # ESP_LOG_WARN, as ConfigManager::create_default() sets it.
    "wifi_ssid": os.environ["WIFI_SSID"],
    "wifi_psk": os.environ["WIFI_PSK"],
    "mqtt_server": os.environ["MQTT_SERVER"],
    "mqtt_port": int(os.environ["MQTT_PORT"]),
    "mqtt_username": os.environ["MQTT_USERNAME"],
    "mqtt_password": os.environ["MQTT_PASSWORD"],
}
with open(sys.argv[1], "w") as f:
    json.dump(config, f)
' "${STAGING}/fs/config.json"

# The filesystem holds only config.json (the web UI is in firmware.bin). Built in a staging
# directory so that no image in .pio/build/ ever contains the passwords.
SPIFFS_SIZE="$(get_partition_field spiffs 5)"
"${MKLITTLEFS}" -c "${STAGING}/fs" -s "$((SPIFFS_SIZE))" -p 256 -b 4096 "${STAGING}/littlefs.bin" >/dev/null

"${ESPTOOL_CMD[@]}" "${CHIP_ARGS[@]}" -o "${OUTPUT}" \
  "${BOOTLOADER_OFFSET}" "${BUILD_DIR}/bootloader.bin" \
  0x8000 "${BUILD_DIR}/partitions.bin" \
  0x10000 "${BUILD_DIR}/firmware.bin" \
  "$(get_partition_field spiffs 4)" "${STAGING}/littlefs.bin" >/dev/null

echo "Wrote ${OUTPUT}: joins '${WIFI_SSID}', MQTT ${MQTT_USERNAME:+${MQTT_USERNAME}@}${MQTT_SERVER}:${MQTT_PORT}."
echo "Flash with: esptool.py write_flash 0x0 ${OUTPUT}"
echo "It contains the Wi-Fi and MQTT passwords. Do not upload it to the manager."
