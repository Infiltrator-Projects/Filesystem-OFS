#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
module=$(realpath "${1:?pass OFS.so}")
boot=$(realpath "${2:?pass the canonical Boot checkout}")
oracle=$(realpath "${3:?pass the pinned amitools source}")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cmp "$root/native/include/infiltratr/fs/boot_module.h" "$boot/include/ifs_boot_module.h"
cc=${CC:-cc}
sanitizers=()
if [[ ${IFS_SO_SANITIZE:-0} == 1 ]]; then
    sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$cc" -std=c11 -O2 -Wall -Wextra -Werror "${sanitizers[@]}" \
    -I"$boot/refind" "$boot/refind/so_elf.c" "$root/tests/ofs_boot_loader_test.c" -o "$work/loader"
python3 "$boot/tests/test_so_malformed.py" "$work/loader" "$module"
prefix=${GNU_EFI_PREFIX:-/usr}
"$cc" -std=c11 -O2 -fshort-wchar -ffunction-sections -fdata-sections "${sanitizers[@]}" \
    -Wall -Wextra -Werror -D__MAKEWITH_GNUEFI -DEFIX64 \
    -I"$prefix/include/efi" -I"$prefix/include/efi/x86_64" -I"$prefix/include/efi/protocol" \
    "$boot/refind/so_elf.c" "$boot/tests/so_uefi_test.c" \
    -Wl,--gc-sections "$prefix/lib/libefi.a" -o "$work/protocol"
for variant in 0 2 4 6; do
for bytes in 512 1024 2048 4096 8192 16384 32768; do
    python3 "$root/tests/ofs_boot_fixture.py" --output "$work/volume.img" --variant "$variant" --block-bytes "$bytes" --oracle "$oracle" --module "$module"
    python3 - "$work/volume.img" "$work/whole.img" <<'PY'
from pathlib import Path
import struct,sys
b=Path(sys.argv[1]).read_bytes();m=bytearray(512);m[450]=0x76
struct.pack_into('<II',m,454,2048,len(b)//512);m[510:512]=b'\x55\xaa'
with open(sys.argv[2],'wb') as f:f.write(m);f.seek(1024*1024);f.write(b)
PY
    before=$(sha256sum "$work/volume.img")
    "$work/loader" "$module" "$work/volume.img" '/Nested Directory/Boot Read Test.bin' "$work/volume.bin"
    "$work/loader" "$module" "$work/volume.img" '/Linked Directory/Boot Read Test.bin' "$work/volume.bin"
    "$work/protocol" "$module" "$work/volume.img" "$work/whole.img"
    test "$(sha256sum "$work/volume.img")" = "$before"
done
done
echo OFS-SAME-SO-BOOT-ELF-UEFI-ALL-BLOCK-SIZES-PASS
