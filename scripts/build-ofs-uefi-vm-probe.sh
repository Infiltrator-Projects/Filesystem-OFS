#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the isolated OVMF acceptance application; never touches an ESP.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
source "$root/scripts/gnu-efi-toolchain.sh"
out="${1:?output directory required}"
mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for mode in supported unsupported; do
    flags=()
    if [[ $mode == unsupported ]]; then
        flags+=(-DOFS_VM_EXPECT_UNSUPPORTED=1)
    fi
    gcc -Os -std=gnu11 -fpic -ffreestanding -fno-builtin -fshort-wchar \
      -fno-stack-protector -fno-strict-aliasing -mno-red-zone \
      -fno-tree-loop-distribute-patterns -Wall -Wextra -Werror \
      -DEFIX64 '-DEFIAPI=__attribute__((ms_abi))' -DEFI_FUNCTION_WRAPPER \
      "${flags[@]}" \
      -I"$gnu_efi_include" -I"$gnu_efi_include/x86_64" \
      -I"$gnu_efi_include/protocol" \
      -c "$root/tests/ofs_uefi_vm_probe.c" -o "$work/$mode.o"
    ld -T "$ldscript" -shared -Bsymbolic -nostdlib -znocombreloc -zdefs \
      "$crt" "$work/$mode.o" "$libefi" "$libgnuefi" \
      "$(gcc -print-libgcc-file-name)" -o "$work/$mode.so"
    objcopy --output-target=efi-app-x86_64 \
      -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rodata \
      -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
      "$work/$mode.so" "$out/BOOTX64-$mode.EFI"
    test -s "$out/BOOTX64-$mode.EFI"
done
