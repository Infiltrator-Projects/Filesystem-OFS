#!/usr/bin/env python3
"""Inspect and exercise extracted package bytes without changing the host OS."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
p=argparse.ArgumentParser();p.add_argument('package',type=Path);p.add_argument('--profile',choices=('portable','aggressive'),required=True)
kernel_options=p.add_mutually_exclusive_group()
kernel_options.add_argument('--kernel-module',type=Path)
kernel_options.add_argument('--kernel-release')
a=p.parse_args();root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'standalone'));import build
with tempfile.TemporaryDirectory(prefix='ofs-package-') as tmp:
    target=Path(tmp);subprocess.run(['dpkg-deb','-x',str(a.package.resolve()),str(target)],check=True)
    payload=[x for x in (target/'System/Filesystems').iterdir()]
    assert len(payload)==1 and payload[0].name=='OFS.so'
    module=payload[0];build.inspect(module)
    docs=target/'System/Resources/Documentation'
    manual=docs/'Filesystem-OFS.pdf'
    assert manual.read_bytes().startswith(b'%PDF-')
    assert {x.name for x in docs.iterdir()} == {'Filesystem-OFS.pdf'}
    report=subprocess.check_output(['dpkg-deb','-f',str(a.package.resolve())],text=True)
    fields=dict(line.split(': ',1) for line in report.splitlines() if ': ' in line)
    assert fields['X-Infiltrator-Native-Profile']==a.profile
    assert fields['X-Infiltrator-Resource-SHA256']==hashlib.sha256(module.read_bytes()).hexdigest()
    assert fields['X-Infiltrator-PDF-SHA256']==hashlib.sha256(manual.read_bytes()).hexdigest()
    assert len(fields['X-Infiltrator-Source-Revision'])==40
    expected_files = {module, manual}
    if a.kernel_module or a.kernel_release:
        kernel = target/'System/Kernel/Filesystems/ofs.ko'
        if a.kernel_module:
            assert kernel.read_bytes() == a.kernel_module.read_bytes()
        if a.kernel_release:
            vermagic = subprocess.check_output(['modinfo','-F','vermagic',str(kernel)],text=True).split()[0]
            assert vermagic == a.kernel_release
            assert fields['X-Infiltrator-Kernel-Release'] == a.kernel_release
        assert fields['X-Infiltrator-Kernel-SHA256'] == hashlib.sha256(kernel.read_bytes()).hexdigest()
        expected_files.add(kernel)
    assert {x for x in target.rglob('*') if x.is_symlink() or not x.is_dir()} == expected_files
    assert all(x.is_file() and not x.is_symlink() for x in expected_files)
    if a.profile=='aggressive':
        assert '-O3 -march=native -mtune=native -flto=auto' in fields['X-Infiltrator-Cflags']
        assert fields['X-Infiltrator-PGO']=='trained-two-pass'
        assert fields['X-Infiltrator-LTO']=='enabled'
    if not a.kernel_module and not a.kernel_release:
        subprocess.run([sys.executable,str(root/'tests/ofs_complete_test.py'),'--module',str(module),'--quick'],check=True)
print('OFS-PACKAGE-CONTENTS-BINARY-IDENTITY-LIFECYCLE-PASS',a.profile)
