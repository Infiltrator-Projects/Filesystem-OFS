#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# End-to-end firmware binding acceptance in isolated QEMU/OVMF.
# No host block devices, mounted ESPs, NVRAM or user's media are touched.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
driver="$(realpath "${1:?diagnostic OFS EFI driver required}")"
work="$(realpath -m "${2:?test work directory required}")"
profile="${3:-diagnostic}"
[[ $profile == diagnostic || $profile == production ]]
mkdir -p "$work"
test -s "$driver"

ovmf=${OFS_OVMF_DIR:-/usr/share/OVMF}
code="$ovmf/OVMF_CODE_4M.fd"
vars="$ovmf/OVMF_VARS_4M.fd"
qemu_data=()
if [[ -n ${OFS_QEMU_DATA:-} ]]; then qemu_data+=(-L "$OFS_QEMU_DATA"); fi
for cmd in qemu-system-x86_64 mkfs.vfat mcopy mmd sgdisk timeout python3; do
    command -v "$cmd" >/dev/null
done
test -s "$code"
test -s "$vars"
bash "$root/scripts/build-ofs-uefi-vm-probe.sh" "$work/efi-probe"

for mode in supported unsupported; do
    case_dir="$work/$mode"
    mkdir -p "$case_dir"
    esp="$case_dir/esp.fat"
    disk="$case_dir/disk.raw"
    ofs="$case_dir/ofs.bin"
    serial="$case_dir/serial.log"
    truncate -s 32M "$esp"
    mkfs.vfat -F 16 -n EFITEST "$esp" >/dev/null
    mmd -i "$esp" ::/EFI
    mmd -i "$esp" ::/EFI/BOOT
    mcopy -i "$esp" "$work/efi-probe/BOOTX64-$mode.EFI" ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$esp" "$driver" ::/OFS_x64.efi

    fixture_args=()
    if [[ $mode == unsupported ]]; then fixture_args+=(--alien); fi
    python3 "$root/tests/ofs_uefi_vm_fixture.py" "$ofs" "${fixture_args[@]}"

    truncate -s 128M "$disk"
    sgdisk --clear --new=1:2048:67583 --typecode=1:ef00 \
      --new=2:67584:68607 --typecode=2:8300 "$disk" >/dev/null
    dd if="$esp" of="$disk" bs=512 seek=2048 conv=notrunc status=none
    dd if="$ofs" of="$disk" bs=512 seek=67584 conv=notrunc status=none
    cp "$vars" "$case_dir/OVMF_VARS.fd"

    echo "OFS OVMF: testing $profile driver on $mode partition"
    set +e
    timeout -k 5s 35s qemu-system-x86_64 "${qemu_data[@]}" \
      -machine q35,accel=tcg -m 256 \
      -display none -monitor none -serial "file:$serial" \
      -nic none -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
      -drive "if=pflash,format=raw,readonly=on,file=$code" \
      -drive "if=pflash,format=raw,file=$case_dir/OVMF_VARS.fd" \
      -drive "if=ide,format=raw,file=$disk" \
      -boot order=c
    result=$?
    set -e
    cat "$serial" || true
    if [[ $result -ne 33 ]]; then
        echo "OFS OVMF $mode: unexpected QEMU exit $result (33 means EFI app success)" >&2
        exit 1
    fi
    if [[ $mode == supported ]]; then
        grep -Fq "OFS_VM_PASS nested read and Stop/rebind" "$serial"
        if [[ $profile == diagnostic ]]; then
            grep -Fq "OFS sector read complete" "$serial"
        fi
    else
        grep -Fq "OFS_VM_PASS unsupported media rejected" "$serial"
        if [[ $profile == diagnostic ]] && grep -Fq "OFS Supported accepted" "$serial"; then
            echo "OFS OVMF: unsupported medium falsely accepted" >&2
            exit 1
        fi
    fi
done
echo "OFS OVMF acceptance: $profile driver read, Stop/rebind and alien-media rejection passed"
