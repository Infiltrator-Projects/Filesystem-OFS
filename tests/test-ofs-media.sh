#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
module=$(realpath "${1:?OFS.so}")
producer=$(realpath "${2:?amitools source}")
kernel=$(realpath "${3:?kernel image}")
initrd=$(realpath "${4:?initramfs}")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd "$work"
# Include room for per-block headers, file extension blocks and the allocation bitmap.
bytes=$(stat -c '%s' "$kernel")
initrd_bytes=$(stat -c '%s' "$initrd")
mib=$((16 + (bytes + initrd_bytes) * 2 / 1048576))
PYTHONPATH="$producer" python3 "$producer/bin/xdftool" clean.hdf create "size=${mib}Mi" \
    + format System ofs + makedir boot + write "$kernel" boot/vmlinuz + write "$initrd" boot/initramfs
cp --sparse=always --reflink=auto clean.hdf bad.hdf
python3 - "$kernel" <<'PY'
import mmap
import sys
with open(sys.argv[1], 'rb') as f:
    sample = f.read(488)
with open('bad.hdf', 'r+b') as f, mmap.mmap(f.fileno(), 0) as disk:
    pos = disk.find(sample)
    if len(sample) != 488 or pos < 24 or pos % 512 != 24 or disk.find(sample, pos + 1) != -1:
        raise ValueError("Expected one OFS data block containing the kernel prefix")
    disk[pos + 487] ^= 1
PY
sha256sum clean.hdf bad.hdf > before
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$root/native/include" "$root/tests/ofs_v2_media_test.c" -ldl -o test
for sector in 512 4096; do
    ./test "$module" clean.hdf "$sector" clean "$kernel" "$initrd"
    ./test "$module" bad.hdf "$sector" bad-data "$kernel" "$initrd"
done
sha256sum -c before
