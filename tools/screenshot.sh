#!/usr/bin/env bash
# Boot the normal (non-test) image headless and save a PNG screenshot of the display.
# Usage: tools/screenshot.sh [out.png] [delay_seconds]
set -eu
OUT=${1:-build/screen.png}
DELAY=${2:-4}
OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
{ sleep "$DELAY"; echo "screendump $OUT -f png"; sleep 1; echo quit; } | \
timeout 60 qemu-system-x86_64 -machine q35 -m 256M -bios "$OVMF" -net none -no-reboot \
    -drive format=raw,file=fat:rw:build/esp -display none -serial file:build/serial-run.log \
    -monitor stdio >/dev/null
[ -s "$OUT" ] && echo "saved $OUT" || { echo "screenshot failed"; exit 1; }
