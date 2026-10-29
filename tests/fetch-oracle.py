#!/usr/bin/env python3
"""Fetch the hash-pinned independent qualification source, never production code."""
import argparse
import hashlib
from pathlib import Path
import tarfile
import urllib.request
URL='https://files.pythonhosted.org/packages/de/01/b837b2fbb7e1dc2a5f5b94149ba59c71293578f2373ae4bb926db0e53c04/amitools-0.8.1.tar.gz'
SHA256='f622c0725c15737e7d4820ed147f930f4cfb7b80b04c786c2bc3943b799faf7e'
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True);archive=a.output/'amitools-0.8.1.tar.gz'
if not archive.exists():
    with urllib.request.urlopen(URL,timeout=60) as source:archive.write_bytes(source.read())
if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA256:raise SystemExit('amitools source hash mismatch')
with tarfile.open(archive,'r:gz') as tar:
    for entry in tar.getmembers():
        parts=Path(entry.name).parts
        if not parts or parts[0]!='amitools-0.8.1' or '..' in parts or entry.issym() or entry.islnk():
            raise SystemExit('Unexpected source archive member')
    tar.extractall(a.output,filter='data')
print(a.output.resolve()/'amitools-0.8.1')
