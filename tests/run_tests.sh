#!/usr/bin/env bash
# Boot TernOS headless under QEMU+OVMF and check the serial log.
#  1. The "test" image: kernel self-tests must pass (exit 33) and every marker in
#     tests/expected.txt must appear.
#  2. Negative tests: each tests/crash-<name>.txt boots build/esp-crash-<name>; the kernel
#     must panic (exit 35) and print every marker in that file.
set -u
BUILD=${1:-build}
here=$(cd "$(dirname "$0")" && pwd)
overall=0

# Keys typed into the guest (QEMU "sendkey" names) once the kernel prints
# "[TEST] kbd: waiting": expected to arrive as "hi T!".
KEYS="h i spc shift-t shift-1 ret"

# Feed QEMU monitor commands on stdout: wait for the kernel's prompt, then type KEYS.
feed_keys() {
    local log=$1
    for _ in $(seq 300); do
        grep -qa "kbd: waiting" "$log" 2>/dev/null && break
        sleep 0.1
    done
    for k in $KEYS; do echo "sendkey $k"; sleep 0.05; done
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

boot test 33 "$here/expected.txt" 0 1
for f in "$here"/crash-*.txt; do
    name=$(basename "$f" .txt)
    boot "$name" 35 "$f" 1
done

if [ $overall -ne 0 ]; then echo "TEST FAILED"; exit 1; fi
echo "ALL TESTS PASSED"
