#!/usr/bin/env python3
"""Boot the actual native KO in an isolated pinned Linux VM and inspect media.

The kernel adapter remains separate from the SO consumer interfaces. This test
never loads a kernel module or mounts a filesystem on the qualification host.
"""
import argparse
import ctypes as C
import hashlib
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
    p.add_argument('--quick', action='store_true')
    p.add_argument('--block-bytes', type=int, choices=(512,1024,2048,4096,8192,16384,32768))
    a = p.parse_args(); sys.path.insert(0, str(a.oracle.resolve()))
    output = a.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    if not a.quick and not a.block_bytes:
        logs = []
        for bs in (512,1024,2048,4096,8192,16384,32768):
            child = output / str(bs)
            subprocess.run([sys.executable, str(Path(__file__).resolve()), *sys.argv[1:],
                            '--block-bytes', str(bs), '--output', str(child)], check=True)
            logs.append((child / 'serial.log').read_text(errors='replace'))
        (output / 'serial.log').write_text('\n'.join(logs))
        print('OFS-NATIVE-KO-ALL28-PROFILES-PASS', flush=True)
        return
    root = output / 'initramfs'; root.mkdir()
    for part in ('bin', 'dev', 'proc', 'sys', 'mnt/test'):
        (root / part).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(shutil.which('busybox'), root / 'bin/busybox')
    (root / 'bin/busybox').chmod(0o755)
    for app in ('sh', 'mount', 'umount', 'mkdir', 'cat', 'echo', 'dd', 'mv', 'sync',
                'poweroff', 'stat', 'test', 'grep', 'ls', 'insmod', 'sha256sum', 'rm', 'rmdir', 'ln', 'chmod', 'readlink'):
        (root / 'bin' / app).symlink_to('busybox')
    shutil.copyfile(a.module.resolve(), root / 'ofs.ko')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-static', '-Wall', '-Wextra',
                    '-Werror', str(Path(__file__).with_name('kernel_vfs_operations.c')),
                    '-o', str(root / 'vfs-operations')], check=True)
    if a.fat16_module: shutil.copyfile(a.fat16_module.resolve(), root / 'fat16.ko')
    from amitools.fs.FSString import FSString
    media = []
    cases = [(a.block_bytes or 512, variant) for variant in (0, 2, 4, 6)]
    api = Api(a.so.resolve())
    for bs, variant in cases:
        blocks = max(4096, bs // 4 + 456) if variant == 6 else 4096
        path = output / f'ofs-{bs}-{variant}.img'; image(path, bs * blocks)
        vol, dev = oracle(path, bs, True, variant)
        vol.write_file(b'independent before kernel', FSString('Producer'))
        if variant == 6 and bs >= 2048:
            length = (bs - 24) * (bs // 4 - 55) + 37
            vol.write_file((bytes(range(256)) * ((length + 255) // 256))[:length], FSString('LargeExtension'))
        vol.close(); dev.close(); media.append(path)
        h = Host(path); assert api.mount(C.byref(h.io), None) == 0
        assert api.mkdir(b'/Aliased') == 0 and api.hardlink(b'/Aliased', b'/DirectoryAlias') == 0
        assert api.symlink(b'VOL:Producer', b'/VolumeLink') == 0
        assert api.unmount() == 0; h.close()
    bad = output / 'wrong-ffs.img'; shutil.copyfile(media[0], bad)
    with bad.open('r+b') as f: f.write(b'DOS\x01')
    def digest(path):
        with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()
    bad_before = digest(bad)
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
echo 'OFS-KO-DEVTMPFS-READY' >/dev/ttyS0
insmod /ofs.ko || fail insmod-ofs
echo 'OFS-KO-REGISTERED' >/dev/ttyS0
mkdir /mnt/second
mount -t ofs -o ro /dev/vdb /mnt/second || fail simultaneous-mount
test "$(cat /mnt/second/Producer)" = 'independent before kernel' || fail simultaneous-read
'''
    for index, (bs, variant) in enumerate(cases):
        def device_name(index):
            suffix = ''
            while index >= 0:
                suffix = chr(97 + index % 26) + suffix
                index = index // 26 - 1
            return '/dev/vd' + suffix
        device = device_name(index)
        init += f'''
echo 'OFS-KO-CASE-BEGIN {bs} {variant} {device}' >/dev/ttyS0
mount -t ofs -o bs={bs} {device} /mnt/test || fail mount-{bs}-{variant}
echo 'OFS-KO-MOUNTED {bs} {variant}' >/dev/ttyS0
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
/vfs-operations create /mnt/test {bs} {variant} || fail extended-create-{bs}-{variant}
cat /vfs-operations >/mnt/test/Execute || fail executable-copy
chmod 755 /mnt/test/Execute || fail executable-mode
/mnt/test/Execute verify /mnt/test {bs} {variant} || fail executable-run-{bs}-{variant}
sync
umount /mnt/test || fail unmount
mount -t ofs -o ro {device} /mnt/test || fail autodetect-readonly
if echo forbidden >/mnt/test/forbidden; then fail readonly-write; fi
mount -t ofs -o remount,rw {device} /mnt/test || fail remount-rw
test "$(cat /mnt/test/tree/Renamed)" = 'OFS native kernel survives remount' || fail readback
test "$(cat /mnt/test/HardAlias)" = 'OFS native kernel survives remount' || fail alias
test "$(stat -c %s /mnt/test/tree/blocks)" = 131072 || fail size
test ! -e /mnt/test/delete || fail persistent-delete
/vfs-operations verify /mnt/test {bs} {variant} || fail extended-verify-{bs}-{variant}
mount -t ofs -o remount,ro {device} /mnt/test || fail remount-ro
if echo forbidden >/mnt/test/forbidden; then fail remount-readonly-write; fi
umount /mnt/test || fail final-unmount
mount -t ofs -o bs={bs},setuid=321,setgid=654,mode=0755,protect,prefix=/Volumes/,volume=OFS:,mufs,verbose {device} /mnt/test || fail presentation-options
test "$(stat -c '%u:%g:%a' /mnt/test/extra/Data)" = '321:654:755' || fail ownership-mode-options
test "$(readlink /mnt/test/VolumeLink)" = '/Volumes/VOL/Producer' || fail symlink-prefix
if echo forbidden >>/mnt/test/extra/Data; then fail protection-option; fi
if mount -t ofs -o remount,rw,mode=0000 {device} /mnt/test; then fail ignored-remount-options; fi
umount /mnt/test || fail options-unmount
echo 'OFS-KO-RUNTIME-PASS {bs} {variant}' >/dev/ttyS0
'''
        if index == 0:
            init += "test \"$(cat /mnt/second/Producer)\" = 'independent before kernel' || fail simultaneous-independent\numount /mnt/second || fail simultaneous-unmount\n"
    if a.fat16_module:
        init += f'''
insmod /fat16.ko || fail insmod-fat16
mount -t fat16 {device_name(len(cases))} /mnt/test || fail fat16-mount
mkdir /mnt/test/tree || fail fat16-mkdir
echo 'FAT16 native kernel survives remount' >'/mnt/test/tree/Long Name.txt' || fail fat16-write
mv '/mnt/test/tree/Long Name.txt' '/mnt/test/tree/Renamed Long Name.txt' || fail fat16-rename
dd if=/dev/zero of=/mnt/test/tree/blocks bs=4096 count=32 || fail fat16-multiblock
echo temporary >/mnt/test/delete || fail fat16-create
rm /mnt/test/delete || fail fat16-delete
sync
umount /mnt/test || fail fat16-unmount
mount -t fat16 {device_name(len(cases))} /mnt/test || fail fat16-remount
test "$(cat '/mnt/test/tree/Renamed Long Name.txt')" = 'FAT16 native kernel survives remount' || fail fat16-readback
test "$(stat -c %s /mnt/test/tree/blocks)" = 131072 || fail fat16-size
test ! -e /mnt/test/delete || fail fat16-persistent-delete
umount /mnt/test || fail fat16-final-unmount
echo 'FAT16-KO-RUNTIME-PASS' >/dev/ttyS0
'''
    init += f"if mount -t ofs {device_name(len(media))} /mnt/test; then fail wrong-format-accepted; fi\n"
    init += "echo 'NATIVE-KO-QUALIFICATION-PASS' >/dev/ttyS0\npoweroff -f\n"
    (root / 'init').write_text(init); (root / 'init').chmod(0o755)
    archive = output / 'initramfs.cpio.gz'
    with archive.open('wb') as target:
        listing = subprocess.run(['find', '.', '-print0'], cwd=root, check=True, stdout=subprocess.PIPE)
        packed = subprocess.run(['cpio', '--null', '-o', '--format=newc'], cwd=root,
                                input=listing.stdout, check=True, stdout=subprocess.PIPE)
        subprocess.run(['gzip', '-9'], input=packed.stdout, stdout=target, check=True)
    command = ['qemu-system-x86_64', '-machine', 'q35', '-m', '512', '-nographic', '-no-reboot', '-nic', 'none',
               '-kernel', str(a.kernel.resolve()), '-initrd', str(archive),
               '-append', 'console=ttyS0 panic=-1']
    if a.qemu_data: command += ['-L', str(a.qemu_data.resolve())]
    for index, path in enumerate([*media, bad]):
        sector = ',logical_block_size=4096,physical_block_size=4096' if index == 2 and cases[0][0] >= 4096 else ''
        command += ['-drive', f'file={path},format=raw,if=none,id=d{index}',
                    '-device', f'virtio-blk-pci,drive=d{index}{sector}']
    with (output / 'serial.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        # At B=32768 the first extension-table boundary exceeds 266 MiB.
        # Full checksum/ownership validation runs repeatedly under software
        # CPU emulation; retain those checks and budget the larger fixture.
        deadline = time.monotonic() + 1200
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
    assert digest(bad) == bad_before, 'Rejected FFS media was modified'
    for path, (bs, variant) in zip(media, cases):
        h = Host(path); assert api.mount(C.byref(h.io), None) == 0
        api.check(); assert api.data(b'/tree/Renamed') == b'OFS native kernel survives remount\n'
        assert api.data(b'/HardAlias') == b'OFS native kernel survives remount\n'
        assert api.unmount() == 0; h.close()
        # This parser cannot enumerate links. Remove only those unsupported
        # entries from a disposable view, then independently read KO output.
        view = path.with_suffix('.oracle-view'); shutil.copyfile(path, view)
        h = Host(view); assert api.mount(C.byref(h.io), None) == 0
        for name in (b'/HardAlias', b'/DirectoryAlias', b'/VolumeLink', b'/extra/Link', b'/extra/ParentLink', b'/extra/LongAlias'):
            assert api.unlink(name) == 0
        assert api.unmount() == 0; h.close()
        vol, dev = oracle(view, bs)
        assert vol.read_file(FSString('tree/Renamed')) == b'OFS native kernel survives remount\n'
        assert vol.read_file(FSString('Aliased/Renamed')) == b'directory alias'
        first = (bs - 24) * (bs // 4 - 56 + 3 if bs <= 1024 else 7) + 37
        want = bytearray((i * 37 + 11) & 255 for i in range(first - 5))
        want.extend(bytes((bs - 24) + 5))
        want[(bs - 24) * 3 - 2:(bs - 24) * 3 + 15] = b'abcdefghijklmnopq'
        for offset, value in ((0,201),(bs - 25,202),(bs - 21,203),(len(want) - 1,204)):
            want[offset] = value
        assert vol.read_file(FSString('extra/Data')) == want
        header = dev.read_block(vol.get_file_path_name(FSString('extra/Data')).block.blk_num)
        assert int.from_bytes(header[bs - 196:bs - 194], 'big') == 123
        assert int.from_bytes(header[bs - 194:bs - 192], 'big') == 456
        long_name = 'N' * (106 if variant == 6 else 30)
        assert vol.get_file_path_name(FSString('extra/' + long_name)).get_meta_info().get_comment().get_ami_str() == b'c' * 79
        if variant == 6 and bs >= 2048:
            length = (bs - 24) * (bs // 4 - 55) + 37
            want = bytearray((bytes(range(256)) * ((length + 255) // 256))[:length])
            want.extend(bytes(bs - 24 + 17))
            offset = (bs - 24) * (bs // 4 - 56) - 7
            want[offset:offset + 17] = b'extension-crossed'
            want[length + bs - 21:length + bs - 17] = b'tail'
            assert hashlib.sha256(vol.read_file(FSString('LargeExtension'))).digest() == hashlib.sha256(want).digest()
        vol.close(); dev.close(); view.unlink()
    if a.fat16_module:
        subprocess.run(['fsck.fat', '-n', str(media[-1])], check=True)
        readback = subprocess.check_output(['mtype', '-i', str(media[-1]), '::/tree/Renamed Long Name.txt'])
        assert readback == b'FAT16 native kernel survives remount\n'
    print('OFS-NATIVE-KO-READ-WRITE-INDEPENDENT-POSTCHECK-PASS', flush=True)


if __name__ == '__main__': main()
