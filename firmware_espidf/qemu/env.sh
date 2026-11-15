# Sourced by run_tests.sh and web_panel.sh: QEMU, mosquitto, the firmware build and the Python venv.
# Sets QEMU_DIR, PROJECT_DIR and VENV, and leaves the working directory at PROJECT_DIR.
# Honours PIO, QEMU_XTENSA, MOSQUITTO, NSPM_QEMU_SKIP_BUILD and NEXTION_TOOLS (see run_tests.sh).

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
# The display emulator comes from nextion-tools, at the release requirements.txt pins, or from a
# local checkout.
REQUIREMENTS=(-r "$QEMU_DIR/requirements.txt")
if [[ -n "${NEXTION_TOOLS:-}" ]]; then
  grep -v '^nextion-tools' "$QEMU_DIR/requirements.txt" > "$VENV/requirements-local.txt"
  REQUIREMENTS=(-r "$VENV/requirements-local.txt" -e "$NEXTION_TOOLS[images]")
fi
if command -v uv >/dev/null; then
  uv pip install -q --python "$VENV/bin/python" "${REQUIREMENTS[@]}"
else
  "$VENV/bin/python" -m pip install -q "${REQUIREMENTS[@]}"
fi
