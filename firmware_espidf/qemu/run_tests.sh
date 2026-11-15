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
#   NSPM_QEMU_GDB       on (1) by default: a failing test writes cpus.txt (see README); 0 turns it off
#   NEXTION_TOOLS=<dir>      use this nextion-tools checkout (installed editable) instead of the
#                            release requirements.txt pins
#
# Results: qemu/build/junit.xml, and per-test console logs in qemu/build/test-logs/.
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

mkdir -p "$QEMU_DIR/build"
rm -rf "$QEMU_DIR/build/test-logs"

# A hang is rare and gone on a rerun, so always be ready to record one.
export NSPM_QEMU_GDB="${NSPM_QEMU_GDB:-1}"

cd "$QEMU_DIR"
exec "$VENV/bin/python" -m pytest tests -v -p no:cacheprovider --junitxml=build/junit.xml "$@"
