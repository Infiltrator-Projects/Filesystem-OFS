#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the transitional native Linux adapter against an explicit kernel ABI.
# The SO is the Boot/Defragmenter engine; the kernel SO bridge is a later step.
set -euo pipefail
if [[ $# -ne 2 ]]; then
    echo 'Usage: build-ofs-kernel.sh OUTPUT_DIRECTORY PREPARED_KERNEL_BUILD_DIRECTORY' >&2
    exit 2
fi
output=$(cd -- "$1" && pwd -P)
kernel=$(cd -- "$2" && pwd -P)
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
test -s "$kernel/Module.symvers" || {
    echo 'The prepared kernel build tree must include Module.symvers.' >&2
    exit 1
}
test ! -e "$output/ofs.ko" && test ! -L "$output/ofs.ko" || {
    echo 'Refusing to overwrite ofs.ko.' >&2
    exit 1
}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/native/filesystems" "$work/native/platform/linux_kernel"
cp -a "$root/native/filesystems/ofs" "$work/native/filesystems/"
cp "$root/native/platform/linux_kernel/vfs_compat.h" "$work/native/platform/linux_kernel/"
make -C "$kernel" M="$work/native/filesystems/ofs/linux" clean
make -C "$kernel" M="$work/native/filesystems/ofs/linux" -j"${OFS_KERNEL_JOBS:-2}" modules
test -s "$work/native/filesystems/ofs/linux/ofs.ko"
install -m 0644 "$work/native/filesystems/ofs/linux/ofs.ko" "$output/ofs.ko"
echo "Built $output/ofs.ko against $kernel"
