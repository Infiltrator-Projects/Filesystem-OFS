#!/usr/bin/env python3
"""Inspect and exercise extracted package bytes without changing the host OS."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
p=argparse.ArgumentParser();p.add_argument('package',type=Path);p.add_argument('--profile',choices=('portable','aggressive'),required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'standalone'));import build
with tempfile.TemporaryDirectory(prefix='ofs-package-') as tmp:
    target=Path(tmp);subprocess.run(['dpkg-deb','-x',str(a.package.resolve()),str(target)],check=True)
    payload=[x for x in (target/'System/Filesystems').iterdir()]
    assert len(payload)==1 and payload[0].name=='OFS.so'
    module=payload[0];build.inspect(module)
    info=(target/'System/Resources/Documentation/Filesystem-OFS/BUILD-INFO').read_text()
    assert 'Profile: '+a.profile+'\n' in info
    assert 'SHA256: '+hashlib.sha256(module.read_bytes()).hexdigest()+'\n' in info
    if a.profile=='aggressive':
        assert '-O3 -march=native -mtune=native -flto=auto' in info
        assert 'PGO: trained two-pass profile' in info
    subprocess.run([sys.executable,str(root/'tests/ofs_complete_test.py'),'--module',str(module),'--quick'],check=True)
print('OFS-PACKAGE-CONTENTS-BINARY-IDENTITY-LIFECYCLE-PASS',a.profile)
