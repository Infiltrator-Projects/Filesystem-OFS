#!/usr/bin/env python3
"""Boot the actual native KO in an isolated pinned Linux VM and inspect media.

The kernel adapter remains separate from the SO consumer interfaces. This test
never loads a kernel module or mounts a filesystem on the qualification host.
"""
import argparse
import ctypes as C
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from ofs_v3_test import Api, Host, oracle, image


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--module', type=Path, required=True)
    p.add_argument('--so', type=Path, required=True)
    p.add_argument('--oracle', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--qemu-data', type=Path)
    p.add_argument('--fat16-module', type=Path)
    a = p.parse_args(); sys.path.insert(0, str(a.oracle.resolve()))
    output = a.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    root = output / 'initramfs'; root.mkdir()
    for part in ('bin', 'dev', 'proc', 'sys', 'mnt/test'):
        (root / part).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(shutil.which('busybox'), root / 'bin/busybox')
    (root / 'bin/busybox').chmod(0o755)
    for app in ('sh', 'mount', 'umount', 'mkdir', 'cat', 'echo', 'dd', 'mv', 'sync',
                'poweroff', 'stat', 'test', 'grep', 'ls', 'insmod', 'sha256sum', 'rm', 'rmdir', 'ln'):
        (root / 'bin' / app).symlink_to('busybox')
    shutil.copyfile(a.module.resolve(), root / 'ofs.ko')
    if a.fat16_module: shutil.copyfile(a.fat16_module.resolve(), root / 'fat16.ko')
    from amitools.fs.FSString import FSString
    media = []
    cases = [(bs, variant) for bs in (512, 1024, 2048, 4096) for variant in (0, 2)]
    for bs, variant in cases:
        path = output / f'ofs-{bs}-{variant}.img'; image(path, bs * 4096)
        vol, dev = oracle(path, bs, True, variant)
        vol.write_file(b'independent before kernel', FSString('Producer'))
        vol.close(); dev.close(); media.append(path)
    if a.fat16_module:
        fat = output / 'fat16.img'; image(fat, 8 * 1024 * 1024)
        subprocess.run(['mkfs.fat', '-F', '16', '-I', '-s', '1', str(fat)], check=True)
        media.append(fat)
    init = '''#!/bin/sh
set -eu
export PATH=/bin
fail() { echo "KO-RUNTIME-FAIL: $*" >/dev/ttyS0; poweroff -f; exit 1; }
mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail dev
insmod /ofs.ko || fail insmod-ofs
'''
    for index, (bs, variant) in enumerate(cases):
        device = '/dev/vd' + chr(97 + index)
        init += f'''
mount -t ofs -o bs={bs} {device} /mnt/test || fail mount-{bs}-{variant}
test "$(cat /mnt/test/Producer)" = 'independent before kernel' || fail producer
mkdir /mnt/test/tree || fail mkdir
echo 'OFS native kernel survives remount' >/mnt/test/tree/Original || fail write
dd if=/dev/zero of=/mnt/test/tree/blocks bs=4096 count=32 || fail multiblock
mv /mnt/test/tree/Original /mnt/test/tree/Renamed || fail rename
ln /mnt/test/tree/Renamed /mnt/test/HardAlias || fail hardlink
echo temporary >/mnt/test/delete || fail create
rm /mnt/test/delete || fail unlink
mkdir /mnt/test/removed || fail mkdir-remove
rmdir /mnt/test/removed || fail rmdir
sync
umount /mnt/test || fail unmount
mount -t ofs -o bs={bs} {device} /mnt/test || fail remount
test "$(cat /mnt/test/tree/Renamed)" = 'OFS native kernel survives remount' || fail readback
test "$(cat /mnt/test/HardAlias)" = 'OFS native kernel survives remount' || fail alias
test "$(stat -c %s /mnt/test/tree/blocks)" = 131072 || fail size
test ! -e /mnt/test/delete || fail persistent-delete
umount /mnt/test || fail final-unmount
echo 'OFS-KO-RUNTIME-PASS {bs} {variant}' >/dev/ttyS0
'''
    if a.fat16_module:
        init += '''
insmod /fat16.ko || fail insmod-fat16
mount -t fat16 /dev/vdi /mnt/test || fail fat16-mount
mkdir /mnt/test/tree || fail fat16-mkdir
echo 'FAT16 native kernel survives remount' >'/mnt/test/tree/Long Name.txt' || fail fat16-write
mv '/mnt/test/tree/Long Name.txt' '/mnt/test/tree/Renamed Long Name.txt' || fail fat16-rename
dd if=/dev/zero of=/mnt/test/tree/blocks bs=4096 count=32 || fail fat16-multiblock
echo temporary >/mnt/test/delete || fail fat16-create
rm /mnt/test/delete || fail fat16-delete
sync
umount /mnt/test || fail fat16-unmount
mount -t fat16 /dev/vdi /mnt/test || fail fat16-remount
test "$(cat '/mnt/test/tree/Renamed Long Name.txt')" = 'FAT16 native kernel survives remount' || fail fat16-readback
test "$(stat -c %s /mnt/test/tree/blocks)" = 131072 || fail fat16-size
test ! -e /mnt/test/delete || fail fat16-persistent-delete
umount /mnt/test || fail fat16-final-unmount
echo 'FAT16-KO-RUNTIME-PASS' >/dev/ttyS0
'''
    init += "echo 'NATIVE-KO-QUALIFICATION-PASS' >/dev/ttyS0\npoweroff -f\n"
    (root / 'init').write_text(init); (root / 'init').chmod(0o755)
    archive = output / 'initramfs.cpio.gz'
    with archive.open('wb') as target:
        listing = subprocess.run(['find', '.', '-print0'], cwd=root, check=True, stdout=subprocess.PIPE)
        packed = subprocess.run(['cpio', '--null', '-o', '--format=newc'], cwd=root,
                                input=listing.stdout, check=True, stdout=subprocess.PIPE)
        subprocess.run(['gzip', '-9'], input=packed.stdout, stdout=target, check=True)
    command = ['qemu-system-x86_64', '-machine', 'q35', '-m', '256', '-nographic', '-no-reboot', '-nic', 'none',
               '-kernel', str(a.kernel.resolve()), '-initrd', str(archive),
               '-append', 'console=ttyS0 panic=-1 quiet']
    if a.qemu_data: command += ['-L', str(a.qemu_data.resolve())]
    for index, path in enumerate(media):
        command += ['-drive', f'file={path},format=raw,if=none,id=d{index}',
                    '-device', f'virtio-blk-pci,drive=d{index}']
    with (output / 'serial.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 180
        try:
            while process.poll() is None and time.monotonic() < deadline:
                serial = (output / 'serial.log').read_text(errors='replace')
                if 'NATIVE-KO-QUALIFICATION-PASS' in serial or 'KO-RUNTIME-FAIL' in serial:
                    break
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                # The tiny qualification kernel can halt without ACPI power
                # off. The guest marker is emitted only after every unmount.
                process.terminate()
            process.wait(timeout=10)
    serial = (output / 'serial.log').read_text(errors='replace')
    assert 'NATIVE-KO-QUALIFICATION-PASS' in serial and 'KO-RUNTIME-FAIL' not in serial, serial[-5000:]
    api = Api(a.so.resolve())
    for path, (bs, variant) in zip(media, cases):
        h = Host(path); assert api.mount(C.byref(h.io), None) == 0
        api.check(); assert api.data(b'/tree/Renamed') == b'OFS native kernel survives remount\n'
        assert api.data(b'/HardAlias') == b'OFS native kernel survives remount\n'
        assert api.unmount() == 0; h.close()
    if a.fat16_module:
        subprocess.run(['fsck.fat', '-n', str(media[-1])], check=True)
        readback = subprocess.check_output(['mtype', '-i', str(media[-1]), '::/tree/Renamed Long Name.txt'])
        assert readback == b'FAT16 native kernel survives remount\n'
    print('OFS-NATIVE-KO-READ-WRITE-INDEPENDENT-POSTCHECK-PASS', flush=True)


if __name__ == '__main__': main()
