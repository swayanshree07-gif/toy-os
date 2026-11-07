#!/usr/bin/env bash
# Boot TernOS headless under QEMU+OVMF and check the serial log.
#  1. Positive images "test" (APIC mode) and "test-pic" (cmdline "noapic": legacy 8259 +
#     PIT): kernel self-tests must pass (exit 33) and every marker in tests/expected.txt
#     plus tests/pass-<image>.txt must appear.
#  2. Negative tests: each tests/crash-<name>.txt boots build/esp-crash-<name>; the kernel
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
#  4. once spin runs: Ctrl-C (kills it, status 130), "status", "exit 3"
KEY_STAGES=(
    "kbd: waiting|h i spc shift-t shift-1 ret"
    "tty: waiting|a b x backspace c ret z z ctrl-u o k spc g o ret ctrl-d"
    "tsh: TernOS shell|e c h o spc h i spc f r o m spc t s h ret c d spc slash e t c ret p w d ret s p i n spc 2 0 0 0 0 ret"
    "20000 ms)|ctrl-c s t a t u s ret y e s spc shift-backslash spc slash b i n slash s p i n spc 3 0 0 0 0 ret"
    "30000 ms)|ctrl-c s t a t u s ret e x i t spc 3 ret"
)

# Feed QEMU monitor commands on stdout.
feed_keys() {
    local log=$1 stage prompt keys
    for stage in "${KEY_STAGES[@]}"; do
        prompt=${stage%%|*}; keys=${stage#*|}
        for _ in $(seq 300); do
            grep -qa "$prompt" "$log" 2>/dev/null && break
            grep -qa "KERNEL PANIC" "$log" 2>/dev/null && return
            sleep 0.1
        done
        sleep 0.2
        for k in $keys; do echo "sendkey $k"; sleep 0.05; done
    done
    # keep the monitor open until the kernel is finished (max ~20 s)
    for _ in $(seq 200); do
        grep -qaE "all done|KERNEL PANIC" "$log" 2>/dev/null && break
        sleep 0.1
    done
}

# boot <image> <expected qemu status> <marker file> <expect panic: 0/1> [keys: 0/1]
boot() {
    local img=$1 want=$2 markers=$3 want_panic=$4 keys=${5:-0}
    local log="$BUILD/serial-$img.log" fail=0
    echo "== $img"
    rm -f "$log"
    # QEMU's vvfat write-back corrupts files when OVMF writes NvVars to the ESP, so
    # every boot gets a throwaway copy of the pristine build output.
    local esp="$BUILD/boot-esp-$img"
    rm -rf "$esp" && cp -r "$BUILD/esp-$img" "$esp"
    local status
    if [ "$keys" = 1 ]; then
        # shellcheck disable=SC2086
        feed_keys "$log" | timeout 60 ${QEMU:-qemu-system-x86_64} $QEMU_COMMON \
            -drive format=raw,file=fat:rw:"$esp" \
            -display none -serial file:"$log" -monitor stdio > "$BUILD/monitor-$img.log"
        status=${PIPESTATUS[1]}
    else
        # shellcheck disable=SC2086
        timeout 60 ${QEMU:-qemu-system-x86_64} $QEMU_COMMON \
            -drive format=raw,file=fat:rw:"$esp" \
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

for img in test test-pic; do
    cat "$here/expected.txt" "$here/pass-$img.txt" > "$BUILD/markers-$img.txt"
    boot "$img" 33 "$BUILD/markers-$img.txt" 0 1
done
for f in "$here"/crash-*.txt; do
    name=$(basename "$f" .txt)
    boot "$name" 35 "$f" 1
done

if [ $overall -ne 0 ]; then echo "TEST FAILED"; exit 1; fi
echo "ALL TESTS PASSED"
