#!/usr/bin/env bash
# Day 30: scripted end-to-end demo of TernOS (`make demo`).
# Boots the release ISO headless with a throwaway copy of the user disk, types a short
# shell session (files, pipes, a disk write, ps/df/uptime), takes PNG screenshots of the
# framebuffer with QEMU's `screendump` (console session, then /bin/demo animation), powers
# off via ACPI and checks the serial log for one marker per step.
# Output: build/demo/{console,animation}.png, build/demo/serial.log
set -u
BUILD=${1:-build}
OUT="$BUILD/demo"
OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}
mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/*.ppm "$OUT/serial.log"
cp "$BUILD/userdisk.img" "$OUT/disk.img"
log="$OUT/serial.log"

# string -> QEMU sendkey names
keys_for() {
    local s=$1 i c out=""
    for ((i = 0; i < ${#s}; i++)); do
        c=${s:i:1}
        case $c in
            [a-z0-9]) out+=" $c" ;;
            [A-Z]) out+=" shift-${c,,}" ;;
            ' ') out+=" spc" ;; '/') out+=" slash" ;; '.') out+=" dot" ;;
            '-') out+=" minus" ;; '>') out+=" shift-dot" ;; '|') out+=" shift-backslash" ;;
            *) echo "demo.sh: no key for '$c'" >&2; exit 2 ;;
        esac
    done
    echo "$out ret"
}

# prompt-to-wait-for | shell command   (a command "!x arg" is a monitor command)
STAGES=(
    "tsh: TernOS shell|cat /disk/welcome.txt"
    "built on the host|ls /disk"
    "welcome.txt|echo hello from day 30 > /disk/notes/day30.txt"
    "day30.txt|cat /disk/notes/day30.txt"
    "^hello from day 30|ls /bin | wc"
    "^ *[0-9]+ +[0-9]+ +[0-9]+|ps"
    "PID|df /disk"
    "free|uptime"
    "up |!screendump $OUT/console.ppm"
    "up |demo 150"
    "demo: running|!screendump $OUT/animation.ppm"
    "demo: 150 frames|poweroff"
)

wait_for() {
    for _ in $(seq 300); do
        grep -qaE -- "$1" "$log" 2>/dev/null && return 0
        grep -qa "KERNEL PANIC" "$log" 2>/dev/null && return 1
        sleep 0.1
    done
    return 1
}

feed() {
    local st prompt cmd k
    for st in "${STAGES[@]}"; do
        prompt=${st%%|*}; cmd=${st#*|}
        wait_for "$prompt" || { echo "demo.sh: timed out waiting for '$prompt'" >&2; return; }
        sleep 0.3
        if [[ $cmd == !* ]]; then echo "${cmd#!}"; sleep 0.7; continue; fi
        for k in $(keys_for "$cmd"); do echo "sendkey $k"; sleep 0.04; done
    done
    wait_for "poweroff: bye"; sleep 2
}

echo "== booting $BUILD/ternos.iso (headless, 60 s limit)"
feed | timeout 60 ${QEMU:-qemu-system-x86_64} -machine q35 -m 256M -bios "$OVMF" -net none \
    -no-reboot -cdrom "$BUILD/ternos.iso" \
    -drive if=none,id=ud,format=raw,file="$OUT/disk.img" -device ide-hd,bus=ide.3,drive=ud \
    -display none -serial file:"$log" -monitor stdio > "$OUT/monitor.log"
status=${PIPESTATUS[1]}

fail=0
check() { if grep -qaE -- "$1" "$log"; then echo "  PASS  $1"; else echo "  FAIL  $1"; fail=1; fi; }
[ "$status" = 0 ] && echo "  PASS  qemu exit status 0 (ACPI poweroff)" || { echo "  FAIL  qemu exit status $status"; fail=1; }
for m in "^fat: disk [0-9] mounted at /disk" "^This image was built on the host" \
         "^hello from day 30" "^ *[0-9]+ +[0-9]+ +[0-9]+" "PID" "free" "up " \
         "^demo: 150 frames" "^poweroff: bye"; do
    check "$m"
done
if [ "$(python3 -I tools/fatimg.py get "$OUT/disk.img" /notes/day30.txt 2>/dev/null)" = "hello from day 30" ] &&
   python3 -I tools/fatimg.py fsck "$OUT/disk.img" > /dev/null; then
    echo "  PASS  host: fatimg reads /notes/day30.txt back, fsck clean"
else echo "  FAIL  host: day30.txt / fsck"; fail=1; fi
grep -qa "KERNEL PANIC" "$log" && { echo "  FAIL  kernel panic"; fail=1; }
for s in console animation; do
    if [ -s "$OUT/$s.ppm" ]; then
        pnmtopng "$OUT/$s.ppm" > "$OUT/$s.png" 2>/dev/null && rm "$OUT/$s.ppm"
        echo "  PASS  screenshot $OUT/$s.png ($(python3 -I tools/ppmstat.py /dev/stdin < <(pngtopnm "$OUT/$s.png" 2>/dev/null) | cut -d' ' -f2-))"
    else
        echo "  FAIL  screenshot $s missing"; fail=1
    fi
done
if [ $fail = 0 ]; then echo "DEMO OK"; else echo "DEMO FAILED (serial log: $log)"; tail -30 "$log"; fi
exit $fail
