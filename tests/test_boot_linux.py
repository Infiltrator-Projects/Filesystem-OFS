#!/usr/bin/env python3
"""Boot the production Boot EFI through the application-facing OFS.so.

Only regular disposable image files are touched. The independently generated OFS volume is accessible only through the staged SO.
No OFS EFI driver or native firmware reader is present.
"""
import argparse
import gzip
import hashlib
from pathlib import Path
import shutil
import stat
import subprocess
from ofs_boot_fixture import make


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def cpio_entry(name, mode, data=b"", major=0, minor=0):
    encoded = name.encode() + b"\0"
    fields = [1, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0]
    output = b"070701" + b"".join(f"{x:08x}".encode() for x in fields) + encoded
    output += b"\0" * (-len(output) % 4)
    return output + data + b"\0" * (-len(data) % 4)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--variant", type=int, choices=(0,2,4,6), default=0)
    parser.add_argument("--block-bytes", type=int, default=512)
    parser.add_argument("--boot", type=Path, required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--busybox", type=Path, default=Path(shutil.which("busybox") or "/bin/busybox"))
    parser.add_argument("--ovmf", type=Path, default=Path("/usr/share/OVMF"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu-data", type=Path)
    parser.add_argument("--cpu", default="max")
    parser.add_argument("--reject-module", action="store_true")
    args = parser.parse_args()
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=True)
    init = b'''#!/bin/sh
/bin/busybox mount -t proc proc /proc
/bin/busybox cat /proc/cmdline
if /bin/busybox grep -q boot_so_test=1 /proc/cmdline; then
 echo BOOT-OFS-SO-LINUX-PASS
else
 echo BOOT-OFS-SO-LINUX-FAIL
fi
/bin/busybox poweroff -f
'''
    archive = b"".join(cpio_entry(d, stat.S_IFDIR | 0o755) for d in ["bin", "dev", "proc"])
    archive += cpio_entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1)
    archive += cpio_entry("bin/busybox", stat.S_IFREG | 0o755, args.busybox.read_bytes())
    archive += cpio_entry("bin/sh", stat.S_IFLNK | 0o777, b"busybox")
    archive += cpio_entry("init", stat.S_IFREG | 0o755, init)
    archive += cpio_entry("TRAILER!!!", 0)
    initrd = work / "initrd.img"
    initrd.write_bytes(gzip.compress(archive, mtime=0))
    config = work / "boot.conf"
    config.write_text('''timeout 1
textonly true
use_nvram false
driver_diagnostics true
scanfor manual
showtools shutdown
menuentry "Linux through OFS.so" {
    volume SOONLY
    filesystem_module ofs
    loader \\vmlinuz
    initrd \\initrd.img
    linux_load memory
    ostype Linux
    graphics off
    options "console=ttyS0,115200 rdinit=/init panic=-1 boot_so_test=1"
}
''')
    esp, media = work / "esp.img", work / "ofs.img"
    for path in [esp, media]:
        with path.open("wb") as file:
            file.truncate(64 * 1024 * 1024)
    run("mkfs.fat", "-F", "32", "-I", "-n", "BOOTESP", esp)
    make(media, args.block_bytes, args.oracle, [("vmlinuz", args.kernel.read_bytes()), ("initrd.img", initrd.read_bytes())], label="SOONLY", variant=args.variant)
    run("mmd", "-i", esp, "::/EFI", "::/EFI/BOOT", "::/EFI/BOOT/filesystems_x64")
    run("mcopy", "-i", esp, args.boot, "::/EFI/BOOT/BOOTX64.EFI")
    run("mcopy", "-i", esp, config, "::/EFI/BOOT/boot.conf")
    module = work / "OFS.so"
    module.write_bytes(args.module.read_bytes() if not args.reject_module else b"invalid ELF" * 64)
    run("mcopy", "-i", esp, module, "::/EFI/BOOT/filesystems_x64/OFS.so")
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    before = digest(media)
    variables = work / "OVMF_VARS.fd"
    shutil.copyfile(args.ovmf / "OVMF_VARS_4M.fd", variables)
    log = work / "serial.log"
    command = ["qemu-system-x86_64", "-machine", "q35,accel=tcg", "-cpu", args.cpu, "-m", "768",
               "-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf / 'OVMF_CODE_4M.fd'}",
               "-drive", f"if=pflash,format=raw,file={variables}",
               "-drive", f"file={esp},format=raw,if=virtio",
               "-drive", f"file={media},format=raw,if=virtio",
               "-nographic", "-serial", f"file:{log}", "-monitor", "none",
               "-no-reboot", "-net", "none"]
    if args.qemu_data:
        command += ["-L", str(args.qemu_data)]
    try:
        subprocess.run(command, timeout=35 if args.reject_module else 120, check=True)
    except subprocess.TimeoutExpired:
        if not args.reject_module:
            raise
    text = log.read_text(errors="replace")
    assert before == digest(media), "Boot changed the filesystem image"
    if args.reject_module:
        assert "BOOT-OFS-SO-LINUX-PASS" not in text, "A broken OFS SO path booted unexpectedly"
        assert "rejected OFS.so" in text, "Missing explicit ELF rejection evidence"
        print("BOOT-INVALID-SO-NO-FALLBACK-PASS")
    else:
        assert "BOOT-OFS-SO-LINUX-PASS" in text, "Linux did not reach our initramfs"
        assert "validated OFS.so" in text and "mounted module" in text
        assert "bytes read through module" in text
        print("BOOT-OFS-SO-LINUX-HANDOFF-PASS", digest(module))


if __name__ == "__main__":
    main()
