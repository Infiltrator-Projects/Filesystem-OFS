#!/usr/bin/env python3
"""Create file bytes independently with pinned amitools; optionally add a V3 directory alias."""
import argparse
from pathlib import Path
import sys
from ofs_v3_test import image,oracle,Api,Host
import ctypes as C

def add_directory_link(path,module):
    a=Api(module.resolve());host=Host(path)
    assert a.mount(C.byref(host.io),None)==0
    assert a.hardlink(b'/Nested Directory',b'/Linked Directory')==0
    a.check();assert a.unmount()==0;host.close()

def make(path,block_bytes,oracle_source,files,label='SOFIXTURE',variant=0):
    sys.path.insert(0,str(Path(oracle_source).resolve()))
    from amitools.fs.FSString import FSString
    image(path,64*1024*1024)
    volume,device=oracle(path,block_bytes,True,variant)
    volume.relabel(FSString(label))
    for name,data in files:
        parts=name.split('/')
        for i in range(1,len(parts)):
            directory='/'.join(parts[:i])
            if volume.get_dir_path_name(FSString(directory)) is None:volume.create_dir(FSString(directory))
        volume.write_file(data,FSString(name))
    volume.close();device.close()

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True)
    p.add_argument('--variant',type=int,choices=(0,2,4,6),default=0);p.add_argument('--block-bytes',type=int,default=512);p.add_argument('--oracle',type=Path,required=True)
    p.add_argument('--module',type=Path)
    args=p.parse_args();data=bytes((i*37+3)&255 for i in range(137123))
    make(args.output,args.block_bytes,args.oracle,[('Nested Directory/Boot Read Test.bin',data),('Nested Directory/Caf\u00e9 \u00dc.bin',data)],variant=args.variant)
    if args.module:add_directory_link(args.output,args.module)
    args.output.with_suffix('.bin').write_bytes(data)

if __name__=='__main__':main()
