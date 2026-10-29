#!/usr/bin/env python3
"""Build one freestanding OFS.so; native profile uses measured disposable I/O."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'native/filesystems/ofs'
SOURCES=[BASE/'module'/n for n in ('ofs_module.c','ofs_v3.c','ofs_boot.c','ofs_memory.c')]+[
    BASE/'core'/n for n in ('ofs_core.c','ofs_primitives.c','ofs_volume.c','ofs_nodes.c',
    'ofs_files.c','ofs_namespace.c','ofs_check.c','ofs_strings.c','ofs_cache.c')]
SOURCES += [BASE/'defrag'/n for n in ('ofs_relayout.c','ofs_defrag_module.c')]
INCLUDES=[BASE/'module',BASE/'core',BASE/'efi',ROOT/'native/include']
EXPORTS=tuple('infiltrator_filesystem_module_v'+str(i) for i in (1,2,3))+('infiltrator_filesystem_boot_v1','infiltrator_filesystem_defrag_v2')

def run(args,**kwargs):subprocess.run([str(x) for x in args],check=True,**kwargs)

def inspect(module):
    if module.name!='OFS.so' or not module.is_file():raise RuntimeError('Missing OFS.so')
    head=subprocess.check_output(['readelf','-h',str(module)],text=True)
    if 'DYN (Shared object file)' not in head or 'Advanced Micro Devices X86-64' not in head:
        raise RuntimeError('Expected x86-64 ELF shared object')
    symbols=subprocess.check_output(['readelf','--dyn-syms','-W',str(module)],text=True)
    exported=[]
    for line in symbols.splitlines():
        fields=line.split()
        if len(fields)<8 or not fields[0].endswith(':'):continue
        if fields[6]=='UND' and fields[0]!='0:':raise RuntimeError('Undefined import: '+line)
        if fields[4]=='GLOBAL' and fields[6]!='UND':exported.append(fields[7])
    if sorted(exported)!=sorted(EXPORTS):raise RuntimeError('Unexpected export set: '+repr(exported))
    dynamic=subprocess.check_output(['readelf','-d',str(module)],text=True)
    if any(x in dynamic for x in ('(NEEDED)','(INIT)','(INIT_ARRAY)','(FINI)','(FINI_ARRAY)','(TEXTREL)')):
        raise RuntimeError('Runtime imports, constructors or text relocations')
    relocs=subprocess.check_output(['readelf','-Wr',str(module)],text=True)
    for line in relocs.splitlines():
        if 'R_X86_64_' in line and 'R_X86_64_RELATIVE' not in line:
            raise RuntimeError('Unsupported relocation: '+line)
    manifest=struct.pack('<8sIIII16s32s',b'IFSBOOT1',72,1,62,1,b'ofs',b'Amiga Original File System')
    if manifest not in module.read_bytes():raise RuntimeError('Missing exact Boot V1 manifest')
    meta=subprocess.check_output(['readelf','-p','.ifsmeta',str(module)],text=True)
    if any('IFS-MODULE:ofs:'+str(i) not in meta for i in (1,2,3)):raise RuntimeError('Missing ABI identities')
    print('OFS-FREESTANDING-BOOT-PROFILE-PASS')

def compile_module(module,flags,training=False):
    args=[os.environ.get('CC','cc'),*flags]
    for inc in INCLUDES:args+=['-I',inc]
    if not training:args+=['-nostdlib']
    args+=['-shared','-Wl,-Bsymbolic','-Wl,-z,defs','-Wl,-z,relro','-Wl,-z,now',
           '-Wl,-z,noexecstack','-o',module,*SOURCES]
    run(args)
    if not training:inspect(module)

def test(module,full=False):
    # These legacy tests call the actual DSO exports; the host provides I/O.
    exe=module.parent/'ofs-module-test'
    run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
         '-I',BASE/'module',ROOT/'tests/ofs_dropin_module_test.c',module,
         '-Wl,-rpath,$ORIGIN','-o',exe]);run([exe])
    args=[sys.executable,ROOT/'tests/ofs_complete_test.py','--module',module]
    if not full:args+=['--quick']
    run(args)

def build(profile,module):
    module=module.resolve();module.parent.mkdir(parents=True,exist_ok=True)
    flags=['-std=c11','-Wall','-Wextra','-Werror','-fPIC','-mno-red-zone','-fno-stack-protector',
        '-fno-builtin','-fno-tree-loop-distribute-patterns','-ffreestanding','-fvisibility=hidden']
    if profile=='portable':flags+=['-O2','-march=x86-64','-mtune=generic']
    else:flags+=['-O3','-march=native','-mtune=native','-flto=auto',
                 '-mno-avx','-mno-avx2','-mno-avx512f','-mno-amx-tile']
    if profile=='portable':compile_module(module,flags)
    else:
        with tempfile.TemporaryDirectory(prefix='ofs-pgo-',dir=module.parent) as tmp:
            profiles=Path(tmp)/'profiles';profiles.mkdir()
            compile_module(module,flags+['-fprofile-generate='+str(profiles)],True)
            test(module)
            if not any(p.stat().st_size for p in profiles.rglob('*.gcda')):
                raise RuntimeError('No measured OFS PGO profile data')
            compile_module(module,flags+['-fprofile-use='+str(profiles),'-fprofile-correction',
                '-fprofile-partial-training'])
        inspect(module)
    return flags

def main():
    p=argparse.ArgumentParser();p.add_argument('--profile',choices=('portable','aggressive'),default='portable')
    p.add_argument('--output',type=Path,default=ROOT/'build/OFS.so');p.add_argument('--check',action='store_true')
    p.add_argument('--inspect-only',action='store_true');args=p.parse_args();module=args.output.resolve()
    if args.inspect_only:inspect(module);return
    flags=build(args.profile,module)
    if args.check:test(module)
    print('QUALIFIED OFS.so',args.profile,' '.join(flags))

if __name__=='__main__':
    try:main()
    except (OSError,RuntimeError,subprocess.CalledProcessError) as exc:sys.exit('Build failed: '+str(exc))
