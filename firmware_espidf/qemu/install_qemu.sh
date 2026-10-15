#!/usr/bin/env bash
# Install Espressif's QEMU fork (qemu-system-xtensa with -M esp32) into
# $NSPM_QEMU_DIR (default ~/.cache/nspm-qemu). Idempotent; prints the binary path.
#
# Runtime libraries it needs on Debian/Ubuntu:
#   libgcrypt20 libglib2.0-0 libpixman-1-0 libsdl2-2.0-0 libslirp0 zlib1g
set -euo pipefail

RELEASE="esp-develop-9.2.2-20260417"
ASSET="qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz"
SHA256="0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35"

DIR="${NSPM_QEMU_DIR:-$HOME/.cache/nspm-qemu}"
BIN="$DIR/qemu/bin/qemu-system-xtensa"
STAMP="$DIR/.installed-$RELEASE"

if [[ -x "$BIN" && -f "$STAMP" ]]; then
  echo "$BIN"
  exit 0
fi

mkdir -p "$DIR"
tarball="$DIR/$ASSET"
curl -fsSL --retry 3 -o "$tarball" "https://github.com/espressif/qemu/releases/download/$RELEASE/$ASSET"
echo "$SHA256  $tarball" | sha256sum -c --quiet -
rm -rf "$DIR/qemu"
tar -xf "$tarball" -C "$DIR"
rm -f "$tarball" "$DIR"/.installed-*
touch "$STAMP"
"$BIN" --version >&2
echo "$BIN"
