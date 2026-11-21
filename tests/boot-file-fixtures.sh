#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Prepare real kernel and initramfs files for a provider-side byte comparison.
set -euo pipefail
out=$(realpath -m "${1:?fixture directory}")
mkdir -p "$out/download" "$out/init/bin"
if [[ $# == 2 ]]; then
    cp -- "$2" "$out/vmlinuz"
else
    image=$(apt-cache depends linux-image-generic | awk '/Depends: linux-image-[0-9]/ { name=$2 } END { print name }')
    test -n "$image"
    (cd "$out/download" && apt-get download "linux-image-unsigned-${image#linux-image-}")
    dpkg-deb -x "$out/download"/linux-image-unsigned-*.deb "$out/download/kernel"
    cp -- "$out/download/kernel/boot/vmlinuz-${image#linux-image-}" "$out/vmlinuz"
fi
cp -- "$(command -v busybox)" "$out/init/bin/busybox"
if readelf -lW "$out/init/bin/busybox" | grep 'INTERP' >/dev/null; then
    echo 'The initramfs requires static busybox' >&2
    exit 1
fi
cat > "$out/init/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox echo "Common V2 filesystem test initramfs"
exec /bin/busybox sh
INIT
chmod 0755 "$out/init/init"
(cd "$out/init" && find . -print0 | LC_ALL=C sort -z | cpio --quiet --null -o -H newc --owner=0:0) | gzip -n > "$out/initramfs"
test -s "$out/vmlinuz" && test -s "$out/initramfs"
