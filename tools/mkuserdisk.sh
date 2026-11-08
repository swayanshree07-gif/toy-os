#!/usr/bin/env bash
# mkuserdisk.sh IMG SRCDIR: new FAT16 image (tools/fatimg.py) with every file and
# directory under SRCDIR copied in (names must be 8.3). Ends with a fatimg fsck.
set -euo pipefail
img=$1 src=$2
fat="python3 -I $(dirname "$0")/fatimg.py"
$fat mkfs "$img.tmp" 8192 TERNUSER
(cd "$src" && find . -mindepth 1 -type d | sort) | while read -r d; do $fat mkdir "$img.tmp" "${d#.}"; done
(cd "$src" && find . -type f | sort) | while read -r f; do $fat put "$img.tmp" "$src/${f#./}" "${f#.}"; done
$fat fsck "$img.tmp" >/dev/null
mv "$img.tmp" "$img"
