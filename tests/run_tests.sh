#!/usr/bin/env bash
# Boot TernOS headless under QEMU+OVMF and check the serial log.
#  1. Positive images "test" (APIC mode) and "test-pic" (cmdline "noapic": legacy 8259 +
#     PIT): kernel self-tests must pass (exit 33) and every marker in tests/expected.txt
#     plus tests/pass-<image>.txt must appear.
#  2. The release ISO (build/ternos.iso, booted with -cdrom): the shell runs /bin/demo
#     (framebuffer animation, screenshotted with QEMU's screendump while it runs and again
#     after it exits) and then /bin/poweroff, so QEMU must exit 0 (ACPI S5).
#  3. Negative tests: each tests/crash-<name>.txt boots build/esp-crash-<name>; the kernel
#     must panic (exit 35) and print every marker in that file.
set -u
BUILD=${1:-build}
here=$(cd "$(dirname "$0")" && pwd)
overall=0

# Keys typed into the guest (QEMU "sendkey" names), in stages: each stage waits for its
# prompt in the serial log, then types its keys.
#  1. kernel keyboard test: expects "hi T!"
#  2. /bin/ttytest: "abx<BS>c", "zz<^U>ok go", then ^D (see user/bin/ttytest.c)
#  3. interactive /bin/tsh: "echo hi from tsh", "cd /etc", "pwd", "spin 20000"
#  4. once spin runs: Ctrl-C (kills it, status 130), "status", "yes | /bin/spin 30000"
#  5. Ctrl-C, "status", "spin 600 &", "jobs", "wait" (background job), "exit 3"
KEY_STAGES=(
    "kbd: waiting|h i spc shift-t shift-1 ret"
    "tty: waiting|a b x backspace c ret z z ctrl-u o k spc g o ret ctrl-d"
    "tsh: TernOS shell|e c h o spc h i spc f r o m spc t s h ret c d spc slash e t c ret p w d ret s p i n spc 2 0 0 0 0 ret"
    "20000 ms)|ctrl-c s t a t u s ret y e s spc shift-backslash spc slash b i n slash s p i n spc 3 0 0 0 0 ret"
    "30000 ms)|ctrl-c s t a t u s ret s p i n spc 6 0 0 spc shift-7 ret j o b s ret w a i t ret e x i t spc 3 ret"
)

# Release ISO session. A key "!cmd:arg" sends the monitor command "cmd arg" instead.
ISO_STAGES=(
    "tsh: TernOS shell|c a t spc slash d i s k slash w e l c o m e dot t x t ret e c h o spc w r i t t e n spc b y spc t e r n o s spc shift-dot spc slash d i s k slash n o t e s slash f r o m o s dot t x t ret l s spc slash d i s k slash n o t e s ret d e m o spc 9 0 ret"
    "demo: running|!screendump:$BUILD/screen-demo.ppm"
    "demo: 90 frames|!screendump:$BUILD/screen-console.ppm"
    "demo: 90 frames|p o w e r o f f ret"
)

# Feed QEMU monitor commands on stdout: feed_keys <log> <stage array name> <end regex>
feed_keys() {
    local log=$1 stage prompt keys k
    local -n stages=$2
    local endre=$3
    for stage in "${stages[@]}"; do
        prompt=${stage%%|*}; keys=${stage#*|}
        for _ in $(seq 300); do
            grep -qa "$prompt" "$log" 2>/dev/null && break
            grep -qa "KERNEL PANIC" "$log" 2>/dev/null && return
            sleep 0.1
        done
        sleep 0.2
        for k in $keys; do
            if [[ $k == !* ]]; then k=${k#!}; echo "${k%%:*} ${k#*:}"; sleep 0.5
            else echo "sendkey $k"; sleep 0.05; fi
        done
    done
    # keep the monitor open until the kernel is finished (max ~20 s)
    for _ in $(seq 200); do
        grep -qaE "$endre|KERNEL PANIC" "$log" 2>/dev/null && break
        sleep 0.1
    done
}

# boot <image> <expected qemu status> <marker file> <expect panic: 0/1>
#      [key stages array name, or 0] [end regex] [media: "esp" or "iso"]
boot() {
    local img=$1 want=$2 markers=$3 want_panic=$4 keys=${5:-0} endre=${6:-all done}
    local media=${7:-esp}
    local log="$BUILD/serial-$img.log" fail=0
    echo "== $img"
    rm -f "$log"
    local drive
    if [ "$media" = iso ]; then
        drive=(-cdrom "$BUILD/ternos.iso"
               -drive if=none,id=ud,format=raw,file="$BUILD/userdisk-rw.img"
               -device ide-hd,bus=ide.3,drive=ud)
    else
        # QEMU's vvfat write-back corrupts files when OVMF writes NvVars to the ESP, so
        # every boot gets a throwaway copy of the pristine build output.
        local esp="$BUILD/boot-esp-$img"
        rm -rf "$esp" && cp -r "$BUILD/esp-$img" "$esp"
        drive=(-drive format=raw,file=fat:rw:"$esp"
               -drive if=none,id=td,format=raw,snapshot=on,file="$BUILD/testdisk.img"
               -device ide-hd,bus=ide.2,drive=td
               -drive if=none,id=fd,format=raw,file="$BUILD/fatdisk-rw.img"
               -device ide-hd,bus=ide.3,drive=fd)
    fi
    local status
    if [ "$keys" != 0 ]; then
        # shellcheck disable=SC2086
        feed_keys "$log" "$keys" "$endre" | timeout 60 ${QEMU:-qemu-system-x86_64} \
            $QEMU_COMMON "${drive[@]}" \
            -display none -serial file:"$log" -monitor stdio > "$BUILD/monitor-$img.log"
        status=${PIPESTATUS[1]}
    else
        # shellcheck disable=SC2086
        timeout 60 ${QEMU:-qemu-system-x86_64} $QEMU_COMMON "${drive[@]}" \
            -display none -serial file:"$log" -monitor none
        status=$?
    fi
    case $status in
        "$want") echo "  PASS  qemu exit status $status" ;;
        124) echo "  FAIL  TIMEOUT after 60 s"; fail=1 ;;
        *) echo "  FAIL  qemu exit status $status (wanted $want)"; fail=1 ;;
    esac
    while IFS= read -r marker; do
        [[ -z "$marker" || "$marker" == \#* ]] && continue
        if grep -qaE -- "$marker" "$log" 2>/dev/null; then
            echo "  PASS  $marker"
        else
            echo "  FAIL  $marker"; fail=1
        fi
    done < "$markers"
    if [ "$want_panic" = 0 ] && grep -qa "KERNEL PANIC" "$log" 2>/dev/null; then
        echo "  FAIL  panic in log:"; grep -a -A2 "KERNEL PANIC" "$log"; fail=1
    fi
    if [ $fail -ne 0 ]; then
        echo "---- serial log ($log) ----"; cat -v "$log" 2>/dev/null | tail -40
        overall=1
    fi
}

# FAT disk checks on the host with the own image tool tools/fatimg.py (Day 17; it is
# cross-checked against mtools in PROGRESS.md, but the suite no longer needs mtools):
# mcheck <description> <command...>
FATIMG="python3 -I $here/../tools/fatimg.py"
mcheck() {
    local what=$1; shift
    if "$@" > "$BUILD/fatimg.out" 2>&1; then echo "  PASS  host: $what"
    else echo "  FAIL  host: $what"; cat "$BUILD/fatimg.out"; overall=1; fi
}
# Both positive boots share one writable copy of the FAT disk (Day 16): boot "test"
# writes persist.txt + pdir/p2.txt, the host reads them with fatimg, boot "test-pic"
# must read them back (and removes them), then the host checks they are gone.
cp "$BUILD/fatdisk.img" "$BUILD/fatdisk-rw.img"
rw="$BUILD/fatdisk-rw.img"
for img in test test-pic; do
    cat "$here/expected.txt" "$here/pass-$img.txt" > "$BUILD/markers-$img.txt"
    boot "$img" 33 "$BUILD/markers-$img.txt" 0 KEY_STAGES
    if [ "$img" = test ]; then
        mcheck "fatimg get /persist.txt" bash -c "$FATIMG get '$rw' /persist.txt | grep -q 'read back by boot 2'"
        mcheck "fatimg get /pdir/p2.txt" bash -c "$FATIMG get '$rw' /pdir/p2.txt | grep -q 'second file in a new directory'"
        mcheck "no scratch files left (w1.dat, newdir, shdir, notes.txt)" bash -c \
            "$FATIMG ls '$rw' / -R > '$BUILD/fatimg.ls' && ! grep -qiE 'w1.dat|newdir|shdir|notes' '$BUILD/fatimg.ls'"
    else
        mcheck "persisted files removed" bash -c "$FATIMG ls '$rw' / -R > '$BUILD/fatimg.ls' && ! grep -qiE 'persist|pdir' '$BUILD/fatimg.ls'"
    fi
    mcheck "big.bin intact (compared with the pristine image)" bash -c \
        "cmp <($FATIMG get '$rw' /big.bin) <($FATIMG get '$BUILD/fatdisk.img' /big.bin)"
    mcheck "fatimg fsck (FAT copies equal, chains, sizes, no lost clusters)" $FATIMG fsck "$rw"
done
# fsck must notice damage: a copy with the second FAT changed must fail.
cp "$rw" "$BUILD/fatdisk-bad.img"
python3 -I -c "
import sys; f=open(sys.argv[1],'r+b'); f.seek(512+32*512+300); f.write(b'\x07\x00')" "$BUILD/fatdisk-bad.img"
mcheck "fatimg fsck rejects a damaged FAT copy" bash -c "! $FATIMG fsck '$BUILD/fatdisk-bad.img'"

# Release ISO: demo + screenshots + poweroff
rm -f "$BUILD"/screen-demo.ppm "$BUILD"/screen-console.ppm
# It gets a copy of the user disk made by tools/fatimg.py (make disk): the shell reads
# a file from it and writes one, which the host then extracts with fatimg.
cp "$BUILD/userdisk.img" "$BUILD/userdisk-rw.img"
boot iso 0 "$here/iso.txt" 0 ISO_STAGES "poweroff:" iso
ud="$BUILD/userdisk-rw.img"
mcheck "fatimg get /notes/fromos.txt (written by the OS)" bash -c \
    "[ \"\$($FATIMG get '$ud' /notes/fromos.txt)\" = 'written by ternos' ]"
mcheck "fatimg fsck of the user disk after the OS wrote to it" $FATIMG fsck "$ud"
# The demo screenshot must be a busy picture (gradient sky, sea, terns), the one after
# it exits the text console again (mostly its background colour 101828).
check_shot() {
    local f=$1 what=$2 cond=$3 out
    if [ ! -f "$f" ]; then echo "  FAIL  screenshot $f missing"; overall=1; return; fi
    out=$(python3 -I "$here/../tools/ppmstat.py" "$f")
    if python3 -I -c "import sys; d=dict(kv.split('=') for kv in sys.argv[1].split()[1:]); sys.exit(0 if ($cond) else 1)" "$out"; then
        echo "  PASS  $what: $out"
    else
        echo "  FAIL  $what: $out"; overall=1
    fi
}
check_shot "$BUILD/screen-demo.ppm" "demo screenshot" "int(d['colors']) > 500 and int(d['top_share']) < 40"
check_shot "$BUILD/screen-console.ppm" "console screenshot after demo" "d['top']=='101828' and int(d['top_share']) > 80"
for f in "$here"/crash-*.txt; do
    name=$(basename "$f" .txt)
    boot "$name" 35 "$f" 1
done

if [ $overall -ne 0 ]; then echo "TEST FAILED"; exit 1; fi
echo "ALL TESTS PASSED"
