#!/usr/bin/env bash
# Visuals pass: boot the release ISO headless, type a short shell session plus /bin/demo,
# and take a QEMU `screendump` every 0.25 s. Frames are turned into docs/media/session.gif
# with ffmpeg; the serial log is kept for tools/plot_boot.py. Real output only.
# Usage: tools/mkmedia.sh [build-dir]
set -u
BUILD=${1:-build}
OUT="$BUILD/media"; MEDIA=docs/media
OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}
rm -rf "$OUT"; mkdir -p "$OUT/frames" "$MEDIA"
cp "$BUILD/userdisk.img" "$OUT/disk.img"
log="$OUT/serial.log"
source <(sed -n '/^keys_for()/,/^}/p' tools/demo.sh)
STAGES=(
    "tsh: TernOS shell|ls /disk"
    "welcome.txt|cat /disk/welcome.txt"
    "built on the host|echo hi from ternos > /disk/hi.txt"
    "> /disk/hi.txt|cat /disk/hi.txt"
    "^hi from ternos|ls /bin | wc"
    "^ *[0-9]+ +[0-9]+ +[0-9]+|ps"
    "PID|demo 120"
    "demo: 120 frames|poweroff"
)
wait_for() { for _ in $(seq 300); do grep -qaE -- "$1" "$log" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
feed() {
    local st p c k
    for st in "${STAGES[@]}"; do
        p=${st%%|*}; c=${st#*|}
        wait_for "$p" || return
        sleep 0.8
        for k in $(keys_for "$c"); do echo "sendkey $k"; sleep 0.06; done
    done
    wait_for "poweroff: bye"; sleep 1
}
shots() { local i=0; sleep 1; while [ $i -lt 400 ]; do printf 'screendump %s/frames/f%04d.ppm\n' "$OUT" $i; i=$((i+1)); sleep 0.25; done; }
{ shots & feed; kill %1 2>/dev/null; } | timeout 90 ${QEMU:-qemu-system-x86_64} -machine q35 -m 256M \
    -bios "$OVMF" -net none -no-reboot -cdrom "$BUILD/ternos.iso" \
    -drive if=none,id=ud,format=raw,file="$OUT/disk.img" -device ide-hd,bus=ide.3,drive=ud \
    -display none -serial file:"$log" -monitor stdio > "$OUT/monitor.log"
echo "frames: $(ls "$OUT/frames" | wc -l)"
# frames -> GIF (640 px wide, 6 fps, shared palette, only changed rectangles re-encoded)
timeout 120 ffmpeg -loglevel error -y -framerate 6 -pattern_type glob -i "$OUT/frames/*.ppm" \
    -vf "scale=640:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" \
    "$MEDIA/session.gif"
# one full-resolution still of the animation, optimised
last=$(ls "$OUT"/frames/*.ppm | tail -12 | head -1)
convert "$last" -strip -resize 640x "$MEDIA/tern-flight.png"
ls -la "$MEDIA"
