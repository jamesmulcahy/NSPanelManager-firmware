#!/usr/bin/env bash
# Build the firmware for QEMU and run the emulated-panel tests.
#
#   qemu/run_tests.sh                 # everything
#   qemu/run_tests.sh -k touch -x     # extra args go to pytest
#
# Environment:
#   PIO                 platformio binary (default: pio on PATH, else ~/.platformio/penv/bin/pio)
#   QEMU_XTENSA         use this qemu-system-xtensa instead of installing one
#   MOSQUITTO           mosquitto binary (default: on PATH, else /usr/sbin/mosquitto)
#   NSPM_QEMU_SKIP_BUILD=1   test the existing .pio/build/qemu images
#
# Results: qemu/build/junit.xml, and per-test console logs in qemu/build/test-logs/.
set -euo pipefail

QEMU_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$QEMU_DIR")"
cd "$PROJECT_DIR"

if [[ -z "${PIO:-}" ]]; then
  if command -v pio >/dev/null; then PIO=pio; else PIO="$HOME/.platformio/penv/bin/pio"; fi
fi

if [[ -z "${MOSQUITTO:-}" ]] && ! command -v mosquitto >/dev/null && [[ ! -x /usr/sbin/mosquitto ]]; then
  echo "mosquitto not found: apt install mosquitto (the tests start their own instance; the system service can stay disabled)" >&2
  exit 2
fi

if [[ -z "${QEMU_XTENSA:-}" ]]; then
  QEMU_XTENSA="$("$QEMU_DIR/install_qemu.sh")"
  export QEMU_XTENSA
fi

if [[ "${NSPM_QEMU_SKIP_BUILD:-}" != "1" ]]; then
  "$PIO" run --environment qemu
  # The harness builds its own LittleFS image (data/ plus a config.json), but
  # buildfs is what makes PlatformIO install mklittlefs.
  "$PIO" run --environment qemu --target buildfs
fi

VENV="$QEMU_DIR/.venv"
if [[ ! -x "$VENV/bin/python" ]]; then
  if command -v uv >/dev/null; then
    uv venv -q --python 3.12 "$VENV"
  else
    python3 -m venv "$VENV"
  fi
fi
if command -v uv >/dev/null; then
  uv pip install -q --python "$VENV/bin/python" -r "$QEMU_DIR/requirements.txt"
else
  "$VENV/bin/python" -m pip install -q -r "$QEMU_DIR/requirements.txt"
fi

mkdir -p "$QEMU_DIR/build"
rm -rf "$QEMU_DIR/build/test-logs"
cd "$QEMU_DIR"
exec "$VENV/bin/python" -m pytest tests -v -p no:cacheprovider --junitxml=build/junit.xml "$@"
