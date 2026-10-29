#!/usr/bin/env python3
"""Actual OFS.so V3 I/O and independent amitools image interoperability.

Only temporary regular-file images are used. The oracle is an external,
pinned test dependency; no oracle filesystem implementation enters OFS.so.
"""
import argparse
import ctypes as C
from datetime import date
import os
from pathlib import Path
import struct
import sys
import tempfile

READ = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.c_void_p, C.c_uint32)
FLUSH = C.CFUNCTYPE(C.c_int, C.c_void_p)
ALLOC = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_size_t)
FREE = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p)


class Date(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('days', 'minutes', 'ticks')]


CLOCK = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(Date))


class Io(C.Structure):
    _fields_ = [('size', C.c_uint32), ('flags', C.c_uint32), ('context', C.c_void_p),
                ('bytes', C.c_uint64), ('read_at', READ), ('write_at', READ),
                ('flush', FLUSH), ('allocate', ALLOC), ('release', FREE), ('clock', CLOCK)]


class Mount(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('size', 'block_bytes', 'reserved', 'root')]


class Format(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('size', 'block_bytes', 'reserved', 'variant')] + [('label', C.c_char_p)]


class Entry(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('size', 'block', 'original', 'bytes')] + [
        ('type', C.c_int32), ('protection', C.c_uint32), ('uid', C.c_uint16), ('gid', C.c_uint16),
        ('changed', Date), ('volume_changed', Date), ('created', Date),
        ('name', C.c_char * 321), ('comment', C.c_char * 241)]


class Space(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('size', 'block_bytes', 'blocks', 'root', 'variant')] + [
        ('total', C.c_uint64), ('free', C.c_uint64), ('valid', C.c_uint32)]


class Check(C.Structure):
    _fields_ = [('size', C.c_uint32), ('errors', C.c_uint32), ('referenced', C.c_uint64),
                ('free', C.c_uint64), ('mismatches', C.c_uint64), ('bad', C.c_uint32), ('reserved', C.c_uint32)]


METHODS = ('mount', 'unmount', 'format', 'stat', 'directory_entry', 'create', 'mkdir', 'read',
           'write', 'truncate', 'rename', 'unlink', 'set_protection', 'set_owner', 'set_comment',
           'set_dates', 'set_label', 'space', 'hardlink', 'symlink', 'readlink', 'verify', 'recover_bitmap', 'lstat', 'flush')


class V3(C.Structure):
    _fields_ = [('version', C.c_uint32), ('size', C.c_uint32), ('format_id', C.c_char_p),
                ('caps', C.c_uint64)] + [(n, C.c_void_p) for n in METHODS]


class Api:
    def __init__(self, module):
        self.lib = C.CDLL(str(module))
        self.lib.infiltrator_filesystem_module_v3.restype = C.POINTER(V3)
        a = self.lib.infiltrator_filesystem_module_v3().contents
        assert a.version == 3 and a.size == C.sizeof(V3) and a.format_id == b'ofs'
        p = C.c_char_p
        signatures = dict(mount=(C.POINTER(Io), C.POINTER(Mount)), unmount=(),
            format=(C.POINTER(Io), C.POINTER(Format)), stat=(p, C.POINTER(Entry)),
            directory_entry=(p, C.c_uint32, C.POINTER(Entry)), create=(p,), mkdir=(p,),
            read=(p,C.c_uint32,C.c_void_p,C.c_uint32), write=(p,C.c_uint32,C.c_void_p,C.c_uint32),
            truncate=(p,C.c_uint32), rename=(p,p), unlink=(p,), set_protection=(p,C.c_uint32),
            set_owner=(p,C.c_uint16,C.c_uint16), set_comment=(p,p), set_dates=(p,C.POINTER(Date),C.c_uint32),
            set_label=(p,), space=(C.POINTER(Space),), hardlink=(p,p), symlink=(p,p), lstat=(p,C.POINTER(Entry)),
            readlink=(p,C.c_void_p,C.c_uint32), verify=(C.POINTER(Check),), recover_bitmap=(C.POINTER(Check),), flush=())
        for n in METHODS:
            assert getattr(a,n), n
            setattr(self,n,C.CFUNCTYPE(C.c_int,*signatures[n])(getattr(a,n)))

    def meta(self, path):
        e=Entry();r=self.stat(path,C.byref(e));assert r==0,('stat',path,r)
        assert e.size==C.sizeof(Entry)
        return e

    def data(self,path):
        e=self.meta(path);out=C.create_string_buffer(e.bytes)
        r=self.read(path,0,out,e.bytes);assert r==0,('read',path,r)
        return out.raw

    def check(self):
        c=Check();r=self.verify(C.byref(c))
        assert r==0,('verify',r,c.bad,c.errors,c.mismatches)
        return c


class Host:
    def __init__(self,path):
        assert path.is_file() and not path.is_symlink()
        self.fd=os.open(path,os.O_RDWR|os.O_NOFOLLOW)
        self.writes=0;self.fail_write=0;self.fail_flush=0;self.flushes=0
        self.now=Date((date(2026,10,29)-date(1978,1,1)).days,12*60+34,56*50+7)
        libc=C.CDLL(None);libc.malloc.argtypes=[C.c_size_t];libc.malloc.restype=C.c_void_p
        libc.free.argtypes=[C.c_void_p]
        def read(_,at,out,n):
            try:
                b=os.pread(self.fd,n,at)
                if len(b)!=n:return -1
                C.memmove(out,b,n);return 0
            except OSError:return -1
        def write(_,at,data,n):
            self.writes+=1
            if self.writes==self.fail_write:return -1
            try:return 0 if os.pwrite(self.fd,C.string_at(data,n),at)==n else -1
            except OSError:return -1
        def flush(_):
            self.flushes+=1
            if self.flushes==self.fail_flush:return -1
            os.fsync(self.fd);return 0
        def clock(_,out):
            out[0]=self.now;return 0
        self.callbacks=(READ(read),READ(write),FLUSH(flush),ALLOC(lambda _,n:libc.malloc(n)),
                        FREE(lambda _,p:libc.free(p)),CLOCK(clock))
        self.io=Io(C.sizeof(Io),0,None,os.fstat(self.fd).st_size,*self.callbacks)

    def close(self):os.close(self.fd)


def image(path,bytes):
    with path.open('wb') as f:f.truncate(bytes)


def oracle(path,bs,create=False,variant=0):
    from amitools.fs.blkdev.BlockDevice import BlockDevice
    from amitools.fs.ADFSVolume import ADFSVolume
    from amitools.fs.FSString import FSString
    class Device(BlockDevice):
        def __init__(self):
            self.fd=os.open(path,os.O_RDWR)
            self._set_geometry(cyls=os.fstat(self.fd).st_size//bs,heads=1,sectors=1,block_bytes=bs)
        def read_block(self,key):return bytearray(os.pread(self.fd,bs,key*bs))
        def write_block(self,key,data):
            # amitools 0.8.1 writes cache type=0 (ADFSDir passes sub_type=0,
            # DirCacheRecord.put also omits it). Correct only this producer
            # defect from each already-written independent entry header.
            if struct.unpack_from('>I',data)[0] == 33:
                data=bytearray(data);at=24
                for _ in range(struct.unpack_from('>I',data,12)[0]):
                    entry=struct.unpack_from('>I',data,at)[0]
                    typ=os.pread(self.fd,4,entry*bs+bs-4)
                    data[at+22]=typ[-1]
                    n=data[at+23];c=data[at+24+n];at+=(26+n+c)&~1
                struct.pack_into('>I',data,20,0)
                checksum=(-sum(struct.unpack('>'+str(bs//4)+'I',data)))&0xffffffff
                struct.pack_into('>I',data,20,checksum)
            assert os.pwrite(self.fd,data,key*bs)==bs
        def close(self):os.close(self.fd)
    dev=Device();vol=ADFSVolume(dev)
    if create:vol.create(FSString('INDEPENDENT'),dos_type=0x444f5300|variant)
    else:vol.open()
    return vol,dev


def lifecycle(api,path,bs,foreign,variant=0):
    from amitools.fs.FSString import FSString
    if foreign:
        vol,dev=oracle(path,bs,True,variant)
        vol.create_dir(FSString('Foreign'))
        vol.write_file(b'written independently',FSString('Foreign/Producer'))
        vol.close();dev.close()
    h=Host(path)
    if not foreign:
        options=Format(C.sizeof(Format),bs,2,variant,b'OURSO')
        r=api.format(C.byref(h.io),C.byref(options));assert r==0,('format',bs,r)
    assert api.mount(C.byref(h.io),None)==0,('mount',bs,foreign)
    root=api.meta(b'/');assert root.type==1
    if foreign:assert api.data(b'/foreign/producer')==b'written independently'
    assert api.mkdir(b'/Parent')==0
    assert api.mkdir(b'/Other')==0
    p=b'/Parent/Caf\xc3\xa9.test'
    assert api.create(p)==0
    payload=bytes((i*37+11)&255 for i in range((bs-24)*3+29))
    r=api.write(p,0,payload,len(payload));assert r==0,('write',bs,r)
    assert api.data(p)==payload
    api.check()
    assert api.set_owner(p,123,456)==0
    assert api.set_comment(p,'Amiga Fran\u00e7ais'.encode())==0
    stamp=Date(12345,1439,2999)
    assert api.set_dates(p,C.byref(stamp),1)==0
    e=api.meta(p);assert (e.uid,e.gid,e.comment)==(123,456,'Amiga Fran\u00e7ais'.encode())
    assert (e.changed.days,e.changed.minutes,e.changed.ticks)==(12345,1439,2999)
    assert api.hardlink(p,b'/Other/Alias')==0
    assert api.data(b'/Other/Alias')==payload
    assert api.symlink(b'Parent/Caf\xc3\xa9.test',b'/Symbol')==0
    assert api.data(b'/Symbol')==payload
    api.check()
    assert api.unlink(p)==0
    assert api.data(b'/Other/Alias')==payload
    api.check()
    assert api.rename(b'/Other/Alias',b'/Parent/Moved')==0
    assert api.truncate(b'/Parent/Moved',5)==0
    assert api.truncate(b'/Parent/Moved',bs+77)==0
    assert api.data(b'/Parent/Moved')==payload[:5]+bytes(bs+72)
    assert api.mkdir(b'/Parent/Child')==0
    assert api.rename(b'/Parent',b'/Parent/Child/LOST')!=0
    link=Entry();assert api.lstat(b'/Symbol',C.byref(link))==0 and link.type==3
    target=C.create_string_buffer(1024)
    assert api.readlink(b'/Symbol',target,len(target))==0 and target.value==b'Parent/Caf\xc3\xa9.test'
    assert api.unlink(b'/Symbol')==0
    api.check()
    assert api.unmount()==0
    assert api.mount(C.byref(h.io),None)==0
    assert api.data(b'/Parent/Moved')==payload[:5]+bytes(bs+72)
    api.check();assert api.unmount()==0
    h.close()
    vol,dev=oracle(path,bs)
    assert vol.read_file(FSString('Parent/Moved'))==payload[:5]+bytes(bs+72)
    vol.write_file(b'oracle reads and writes this volume',FSString('OracleRoundTrip'))
    vol.close();dev.close()
    h=Host(path);assert api.mount(C.byref(h.io),None)==0
    assert api.data(b'/oracleroundtrip')==b'oracle reads and writes this volume'
    api.check();assert api.unmount()==0;h.close()


def long_name_reader(api,path,bs):
    # SO-created LNFS names/comments read by the independent consumer.
    from amitools.fs.FSString import FSString
    image(path,max(8*1024*1024,bs*1024));h=Host(path)
    options=Format(C.sizeof(Format),bs,2,6,b'LNFS')
    assert api.format(C.byref(h.io),C.byref(options))==0
    assert api.mount(C.byref(h.io),None)==0
    names=['N'*n for n in (30,31,106)]
    for name in names:
        p=('/'+name).encode();assert api.create(p)==0
        assert api.write(p,0,b'LNFS data',9)==0 and api.set_comment(p,b'c'*79)==0
    api.check();assert api.unmount()==0;h.close()
    vol,dev=oracle(path,bs)
    for name in names:
        node=vol.get_file_path_name(FSString(name))
        assert node is not None and vol.read_file(FSString(name))==b'LNFS data'
        assert node.get_meta_info().get_comment().get_ami_str()==b'c'*79
    vol.close();dev.close()
    # A raw 107-byte reader fixture is distinct from independent production.
    # Rename a single 106-byte entry and update its authoritative hash bucket.
    h=Host(path);assert api.mount(C.byref(h.io),None)==0
    for name in names[:2]:assert api.unlink(('/'+name).encode())==0
    key=api.meta(('/'+names[-1]).encode()).block
    s=Space();assert api.space(C.byref(s))==0;root=s.root
    assert api.unmount()==0;h.close()
    with path.open('r+b') as f:
        f.seek(key*bs);b=bytearray(f.read(bs));new=b'N'*107
        b[bs-184]=107;b[bs-183:bs-183+107]=new;b[bs-76]=0
        def checksum(data):
            struct.pack_into('>I',data,20,0)
            struct.pack_into('>I',data,20,(-sum(struct.unpack('>'+str(bs//4)+'I',data)))&0xffffffff)
        checksum(b);f.seek(key*bs);f.write(b)
        f.seek(root*bs);rb=bytearray(f.read(bs));rb[24:bs-200]=bytes(bs-224)
        hash=107
        for c in new:hash=(hash*13+c)&0x7ff
        struct.pack_into('>I',rb,24+4*(hash%(bs//4-56)),key);checksum(rb)
        f.seek(root*bs);f.write(rb)
    h=Host(path);assert api.mount(C.byref(h.io),None)==0
    assert api.data(b'/'+new)==b'LNFS data';api.check()
    assert api.unmount()==0;h.close()
    vol,dev=oracle(path,bs);assert vol.read_file(FSString(new))==b'LNFS data'
    vol.close();dev.close()
    print('OFS-LNFS-LONG-NAME-COMMENT-INDEPENDENT-READER-PASS',bs,flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('--module',type=Path,required=True)
    p.add_argument('--oracle',type=Path,required=True);p.add_argument('--quick',action='store_true')
    args=p.parse_args();sys.path.insert(0,str(args.oracle.resolve()))
    api=Api(args.module.resolve())
    with tempfile.TemporaryDirectory(prefix='ofs-so-v3-') as tmp:
        for bs in ((512,) if args.quick else (512,1024,2048,4096,8192,16384,32768)):
            for variant in (0,2,4,6):
                for foreign in (False,True):
                    path=Path(tmp)/('foreign' if foreign else 'own')
                    image(path,max(8*1024*1024,bs*1024))
                    lifecycle(api,path,bs,foreign,variant)
                    path.unlink()
                    print('OFS-SO-INDEPENDENT-ROUNDTRIP-PASS',bs,variant,foreign,flush=True)
            long_name_reader(api,Path(tmp)/'long-name.img',bs)
    print('OFS-SO-V3-LIFECYCLE-INDEPENDENT-PASS')


if __name__=='__main__':main()
