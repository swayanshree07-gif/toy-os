#!/usr/bin/env bash
# Boot TernOS headless under QEMU+OVMF with the "test" kernel cmdline, capture the
# serial log and check it for every marker in tests/expected.txt.
set -u
BUILD=${1:-build}
LOG="$BUILD/serial-test.log"
ESP="$BUILD/esp-test"
here=$(cd "$(dirname "$0")" && pwd)

rm -f "$LOG"
# shellcheck disable=SC2086
timeout 60 ${QEMU:-qemu-system-x86_64} $QEMU_COMMON \
    -drive format=raw,file=fat:rw:"$ESP" \
    -display none -serial file:"$LOG" -monitor none
status=$?

fail=0
case $status in
    33) echo "qemu: kernel reported success (exit 33)" ;;
    35) echo "qemu: kernel reported FAILURE (exit 35)"; fail=1 ;;
    124) echo "qemu: TIMEOUT after 60 s"; fail=1 ;;
    *) echo "qemu: unexpected exit status $status"; fail=1 ;;
esac

while IFS= read -r marker; do
    [[ -z "$marker" || "$marker" == \#* ]] && continue
    if grep -qaE -- "$marker" "$LOG" 2>/dev/null; then
        echo "  PASS  $marker"
    else
        echo "  FAIL  $marker"
        fail=1
    fi
done < "$here/expected.txt"

if grep -qa "KERNEL PANIC" "$LOG" 2>/dev/null; then
    echo "  FAIL  panic in log:"; grep -a -A2 "KERNEL PANIC" "$LOG"; fail=1
fi

if [ $fail -ne 0 ]; then
    echo "---- serial log ($LOG) ----"; cat -v "$LOG" 2>/dev/null | tail -40
    echo "TEST FAILED"; exit 1
fi
echo "ALL TESTS PASSED"
