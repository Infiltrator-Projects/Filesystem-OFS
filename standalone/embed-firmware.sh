#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Embed the separately built firmware ELF without executing it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
module=$(realpath "${1:?hosted module required}")
[[ $(basename "$module") == OFS.so && -f "$module" && ! -L "$module" ]]
bash "$root/standalone/prepare-common.sh"
common="$root/src/infiltratr-common/include"
test "$(git hash-object "$common/infiltratr/filesystem_module_v2.h")" = 90730b68a53ecf6a6e304c789501f29c17c728c7
work=$(mktemp -d "$(dirname "$module")/.firmware-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
cmake -S "$root/module/firmware" -B "$work/build" -DCOMMON_INCLUDE_DIR="$common"
cmake --build "$work/build"
firmware="$work/build/OFS.so"
test -s "$firmware"
test "$(nm -D --defined-only "$firmware" | awk '{print $3}')" = infiltrator_filesystem_api_v2
test -z "$(nm -D --undefined-only "$firmware")"
! readelf -dW "$firmware" | grep -E 'NEEDED|SONAME|INIT_ARRAY|FINI_ARRAY|RELR'
objdump -d "$firmware" > "$work/disassembly"
! grep -E '(%[xyz]mm[0-9]+|%st\(|%mm[0-9]+|%k[0-7]([,[:space:]]|$))' "$work/disassembly"
objcopy --add-section ".infiltrator_boot=$firmware" --set-section-flags .infiltrator_boot=readonly "$module" "$work/carrier.so"
objcopy --dump-section ".infiltrator_boot=$work/extracted.so" "$work/carrier.so" "$work/verify-carrier.so"
cmp "$firmware" "$work/extracted.so"
mv -f "$work/carrier.so" "$module"
