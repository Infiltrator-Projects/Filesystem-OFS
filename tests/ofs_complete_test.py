#!/usr/bin/env python3
"""OFS.so boundary, allocation, metadata, corruption and failed-I/O checks.

All media are disposable regular files. Raw edits are specification-based
negative/boundary fixtures, not evidence of external-creator interoperability.
"""
import argparse
import ctypes as C
import hashlib
import os
from pathlib import Path
import shutil
import struct
import tempfile
from ofs_v3_test import Api, Host, Format, Mount, Entry, Space, Check, Date, Io, READ, image

SIZES=(512,1024,2048,4096,8192,16384,32768)

def u32(data,at):return struct.unpack_from('>I',data,at)[0]
def put(data,at,value):struct.pack_into('>I',data,at,value)
def checksum(data,at=20):
    put(data,at,0);put(data,at,(-sum(struct.unpack('>'+str(len(data)//4)+'I',data)))&0xffffffff)
def block(path,key,bs):
    with path.open('rb') as f:f.seek(key*bs);return bytearray(f.read(bs))
def store(path,key,bs,data):
    with path.open('r+b') as f:f.seek(key*bs);f.write(data)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def space(a):
    s=Space();assert a.space(C.byref(s))==0;return s

def fresh(a,path,bs=512,variant=0,blocks=4096,reserved=2,label=b'BOUNDARY'):
    image(path,bs*blocks);h=Host(path)
    fmt=Format(C.sizeof(Format),bs,reserved,variant,label)
    assert a.format(C.byref(h.io),C.byref(fmt))==0
    opt=Mount(C.sizeof(Mount),0,reserved,0)
    r=a.mount(C.byref(h.io),C.byref(opt));assert r==0,('fresh',bs,variant,r)
    return h

def metadata(a,path,bs,variant):
    h=fresh(a,path,bs,variant,label=b'')
    assert a.meta(b'/').name==b''
    assert a.set_label('Fran\u00e7ais'.encode())==0 and a.meta(b'/').name=='Fran\u00e7ais'.encode()
    assert a.set_label(b'')==0 and a.meta(b'/').name==b''
    assert a.set_label('Fran\u00e7ais'.encode())==0
    stamp=Date(123,45,678)
    for mask,field in ((1,'changed'),(2,'volume_changed'),(4,'created')):
        assert a.set_dates(b'/',C.byref(stamp),mask)==0
        e=a.meta(b'/');d=getattr(e,field);assert (d.days,d.minutes,d.ticks)==(123,45,678),(variant,mask)
    limit=106 if variant==6 else 30
    p=b'/'+b'N'*limit
    assert a.create(p)==0 and a.create(p+b'x')==-1
    assert a.set_comment(p,b'c'*79)==0 and a.meta(p).comment==b'c'*79
    c=a.check()
    if variant==6:
        key=a.meta(p).block;header=block(path,key,bs);comment=u32(header,bs-72)
        assert comment and u32(block(path,comment,bs),8)==key
        assert a.rename(p,b'/short')==0
        assert a.meta(b'/short').comment==b'c'*79
        header=block(path,key,bs);assert u32(header,bs-72)==0
        assert a.check().referenced==c.referenced-1
        assert a.rename(b'/short',p)==0 and a.check().referenced==c.referenced
        assert a.set_comment(p,b'')==0 and a.check().referenced==c.referenced-1
    assert a.write(p,0,b'abc',3)==0
    assert a.set_protection(p,0xffffffff)==0
    out=C.create_string_buffer(3)
    assert a.read(p,0,out,3)==-6 and a.write(p,0,b'X',1)==-6 and a.unlink(p)==-6
    assert a.set_protection(p,4)==0
    assert a.unlink(p)==0 # Delete and write protection are independent.
    lower='/\u00e4case'.encode();upper='/\u00c4CASE'.encode()
    assert a.create(lower)==0
    assert a.create(upper)==(0 if variant==0 else -5)
    assert a.mkdir(b'/D')==0 and a.mkdir(b'/D/E')==0
    assert a.create(b'/D/F')==0 and a.write(b'/D/F',0,b'hello',5)==0
    for target,link in ((b'F',b'/D/local'),(b'/D/F',b'/D/rooted'),
                        ('Fran\u00e7ais:D/F'.encode(),b'/D/colon'),(b'/D//D/F',b'/D/parent')):
        assert a.symlink(target,link)==0,(target,link)
        assert a.data(link)==b'hello',(target,link)
    assert a.symlink(b'loop',b'/D/loop')==0
    e=Entry();assert a.stat(b'/D/loop',C.byref(e))==-3
    assert a.mkdir(b'/else')==0
    assert a.hardlink(b'/D/F',b'/else/A')==0 and a.hardlink(b'/D/F',b'/else/B')==0
    assert a.set_comment(b'/else/A',b'c'*79)==0
    assert a.meta(b'/D/F').comment==a.meta(b'/else/A').comment==a.meta(b'/else/B').comment==b'c'*79
    assert a.unlink(b'/D/F')==0 and a.data(b'/else/A')==b'hello'
    assert a.unlink(b'/else/A')==0 and a.data(b'/else/B')==b'hello'
    assert a.rename(b'/else/B',b'/D/last')==0
    assert a.create(b'/replacement')==0 and a.write(b'/replacement',0,b'new',3)==0
    assert a.rename(b'/replacement',b'/D/last')==0 and a.data(b'/D/last')==b'new'
    assert a.rename(b'/D',b'/D/E/cycle')==-6
    a.check();assert a.unmount()==0;h.close()
    print('OFS-METADATA-LINKS-VARIANT-PASS',bs,variant,flush=True)

def extension(a,path,bs):
    h=fresh(a,path,bs);assert a.create(b'/large')==0
    payload=bs-24;table=bs//4-56
    data=bytes((i*19+3)&255 for i in range(payload*(table*2+3)+37))
    assert a.write(b'/large',0,data,len(data))==0
    assert a.data(b'/large')==data
    key=a.meta(b'/large').block;hdr=block(path,key,bs)
    ext=u32(hdr,bs-8);assert ext and u32(block(path,ext,bs),bs-8)
    a.check()
    for n in (payload*table+1,payload*table,payload*table-1,payload+1,payload,1,0):
        assert a.truncate(b'/large',n)==0 and a.data(b'/large')==data[:n]
        a.check()
    assert a.write(b'/large',payload*2+7,b'END',3)==0
    assert a.data(b'/large')==bytes(payload*2+7)+b'END'
    assert a.read(b'/large',payload*2+9,C.create_string_buffer(2),2)==-1
    assert a.write(b'/large',0xffffffff,b'X',1)==-1
    assert a.unmount()==0;h.close();print('OFS-REVERSED-EXTENSIONS-ZERO-FILL-PASS',bs,flush=True)

def directory_links(a,path,bs,variant):
    h=fresh(a,path,bs,variant)
    assert a.mkdir(b'/D')==0 and a.mkdir(b'/D/Sub')==0 and a.mkdir(b'/E')==0
    assert a.create(b'/D/Sub/F')==0 and a.write(b'/D/Sub/F',0,b'kept',4)==0
    original=a.meta(b'/D').block
    assert a.hardlink(b'/D',b'/E/A')==0
    assert a.hardlink(b'/E/A',b'/B')==0
    assert a.meta(b'/E/A').type==4 and a.meta(b'/E/A').original==original
    # Check native link records independently of path resolution.
    alias=a.meta(b'/B').block;hdr=block(path,alias,bs)
    assert u32(hdr,bs-4)==4 and u32(hdr,bs-44)==original
    assert u32(block(path,original,bs),bs-40)==alias
    assert a.create(b'/E/A/Shared')==0 and a.write(b'/B/Shared',0,b'shared',6)==0
    assert a.data(b'/D/Shared')==b'shared'
    assert a.set_comment(b'/B',b'object comment')==0
    assert a.meta(b'/D').comment==a.meta(b'/B').comment==a.meta(b'/E/A').comment==b'object comment'
    assert a.set_comment(b'/E/A',b'c'*79)==0
    assert a.meta(b'/B').comment==b'c'*79
    assert a.set_comment(b'/B',b'object comment')==0
    # Native explicit hard links may form loops; enumeration/verification
    # follows each original once, without recursion or an arbitrary entry cap.
    assert a.hardlink(b'/D',b'/D/Sub/Cycle')==0
    assert a.data(b'/D/Sub/Cycle/Sub/Cycle/Shared')==b'shared'
    a.check()
    assert a.rename(b'/D/Sub/Cycle',b'/D/Sub/MovedCycle')==0
    assert a.unlink(b'/D')==0 # Skip the newest cyclic alias for promotion.
    assert a.meta(b'/B').block==original and a.meta(b'/B').type==2
    assert a.data(b'/E/A/Sub/F')==b'kept'
    assert u32(block(path,a.meta(b'/B/Sub').block,bs),bs-12)==original
    assert a.meta(b'/B').comment==b'object comment'
    a.check();assert a.unmount()==0;h.close()
    h=Host(path);assert a.mount(C.byref(h.io),None)==0
    assert a.unlink(b'/B')==0 and a.meta(b'/E/A').block==original
    assert a.data(b'/E/A/Sub/MovedCycle/Sub/F')==b'kept'
    # Removing the sole rooted name would orphan the tree; reject it before
    # dirtying media. The user may remove the internal alias first.
    before=digest(path);writes=h.writes
    assert a.unlink(b'/E/A')==-6 and digest(path)==before and h.writes==writes
    assert a.hardlink(b'/',b'/RootAlias')==-1
    assert a.unlink(b'/E/A/Sub/MovedCycle')==0
    assert a.unlink(b'/E/A/Sub/F')==0 and a.unlink(b'/E/A/Sub')==0
    assert a.unlink(b'/E/A/Shared')==0 and a.unlink(b'/E/A')==0
    assert a.unlink(b'/E')==0
    a.check();assert a.unmount()==0;h.close()
    print('OFS-DIRECTORY-HARDLINK-MUTATION-PROMOTION-CYCLES-PASS',bs,variant,flush=True)

def full_directory_promotion(a,path):
    h=fresh(a,path,512,6,32)
    assert a.mkdir(b'/D')==0 and a.create(b'/D/child')==0
    assert a.write(b'/D/child',0,b'preserved child',15)==0
    assert a.set_comment(b'/D',b'c'*79)==0
    alias=b'/'+b'A'*106
    assert a.hardlink(b'/D',alias)==0 and a.create(b'/fill')==0
    free=space(a).free//512
    assert a.truncate(b'/fill',free*488)==0 and space(a).free==0
    key=a.meta(b'/D').block
    assert a.unlink(b'/D')==0
    assert a.meta(alias).block==key and a.meta(alias).comment==b'c'*79
    assert a.data(alias+b'/child')==b'preserved child'
    a.check();assert a.unmount()==0;h.close()
    print('OFS-FULL-LNFS-DIRECTORY-PROMOTION-COMMENT-REUSE-PASS',flush=True)

def many_entries(a,path,variant,count=600):
    h=fresh(a,path,512,variant,8192)
    assert a.mkdir(b'/Many')==0
    for i in range(count):
        p=f'/Many/file-{i:04}'.encode();r=a.create(p);assert r==0,(variant,i,r)
    names=set()
    for i in range(count+1):
        e=Entry();r=a.directory_entry(b'/Many',i,C.byref(e))
        if i==count:assert r==1
        else:assert r==0;names.add(e.name)
    assert len(names)==count
    assert a.rename(f'/Many/file-{count-1:04}'.encode(),b'/Outside')==0
    assert a.set_comment(b'/Many/file-0001',b'x'*79)==0
    a.check()
    for i in range(count):
        if i!=count-1:assert a.unlink(f'/Many/file-{i:04}'.encode())==0
    assert a.unlink(b'/Many')==0
    a.check();assert a.unmount()==0;h.close()
    print('OFS-DIRECTORY-ENTRIES-COLLISIONS-CACHE-PASS',count,variant,flush=True)

def full_media(a,path,variant):
    h=fresh(a,path,512,variant,16)
    assert a.create(b'/fill')==0
    free=space(a).free//512
    assert a.truncate(b'/fill',free*488)==0
    assert space(a).free==0
    before=digest(path);writes=h.writes
    assert a.create(b'/no-room')==-4
    assert a.write(b'/fill',free*488,b'X',1)==-4
    assert before==digest(path) and h.writes==writes
    assert a.unlink(b'/fill')==0
    a.check();assert a.unmount()==0;h.close()
    print('OFS-FULL-VOLUME-PREFLIGHT-PASS',variant,flush=True)

def corruption(a,path):
    h=fresh(a,path);assert a.create(b'/file')==0
    assert a.write(b'/file',0,b'A'*1100,1100)==0
    key=a.meta(b'/file').block;root=space(a).root
    data=u32(block(path,key,512),308)
    assert a.unmount()==0;h.close();base=path.read_bytes()
    cases=[('header-count',key,8,99),('first-data',key,16,key),
           ('pointer-self',key,308,key),('extension-self',key,504,key),
           ('data-owner',data,4,root),('data-sequence',data,8,99),
           ('data-size',data,12,1),('data-next',data,16,data)]
    for name,k,at,v in cases:
        path.write_bytes(base);b=block(path,k,512);put(b,at,v);checksum(b);store(path,k,512,b)
        h=Host(path);assert a.mount(C.byref(h.io),None)==-3
        h.io.flags=1;assert a.mount(C.byref(h.io),None)==0
        writes=h.writes;out=C.create_string_buffer(1100)
        assert a.read(b'/file',0,out,1100)!=0,name
        assert a.write(b'/file',0,b'Z',1)!=0,name
        assert h.writes==writes,name
        c=Check();assert a.verify(C.byref(c))==-3,name
        assert a.unmount()==0;h.close()
    path.write_bytes(base)
    bm=u32(block(path,root,512),316)
    b=block(path,bm,512);b[100]^=1;store(path,bm,512,b)
    h=Host(path);assert a.mount(C.byref(h.io),None)==-3
    h.io.flags=2;assert a.mount(C.byref(h.io),None)==0
    before=h.writes;c=Check();assert a.recover_bitmap(C.byref(c))==0
    assert h.writes>before and a.data(b'/file')==b'A'*1100
    a.check();assert a.unmount()==0;h.close()
    # A namespace cycle is structural damage, not permission to guess repair.
    path.write_bytes(base);b=block(path,key,512);put(b,496,key);checksum(b);store(path,key,512,b)
    h=Host(path);h.io.flags=2;assert a.mount(C.byref(h.io),None)==0
    before=h.writes;c=Check();assert a.recover_bitmap(C.byref(c))==-3 and h.writes==before
    assert a.unmount()==0;h.close()
    print('OFS-CORRUPTION-REJECTION-EXPLICIT-BITMAP-RECOVERY-PASS',flush=True)

def faults(a,path):
    h=fresh(a,path);assert a.create(b'/file')==0
    assert a.write(b'/file',0,b'old',3)==0
    assert a.unmount()==0;h.close();base=path.read_bytes()
    # Measure an actual operation, then fail every write and every barrier.
    h=Host(path);assert a.mount(C.byref(h.io),None)==0
    start=h.writes;startf=h.flushes
    assert a.write(b'/file',0,b'new',3)==0
    count=h.writes-start;flushes=h.flushes-startf
    assert a.unmount()==0;h.close()
    for kind,total in (('write',count),('flush',flushes)):
        for nth in range(1,total+1):
            path.write_bytes(base);h=Host(path);assert a.mount(C.byref(h.io),None)==0
            if kind=='write':h.fail_write=nth
            else:h.fail_flush=nth
            assert a.write(b'/file',0,b'new',3)==-2,(kind,nth)
            h.fail_write=h.fail_flush=0;assert a.unmount()==0;h.close()
            h=Host(path);r=a.mount(C.byref(h.io),None)
            if r==-3:
                h.io.flags=2;assert a.mount(C.byref(h.io),None)==0
                assert a.write(b'/file',0,b'X',1)==-6
                c=Check();assert a.recover_bitmap(C.byref(c))==0,(kind,nth,c.bad)
            else:assert r==0,(kind,nth,r)
            assert a.data(b'/file') in (b'old',b'new')
            a.check();assert a.unmount()==0;h.close()
    print('OFS-EVERY-OVERWRITE-WRITE-AND-FLUSH-FAULT-PASS',count,flushes,flush=True)

def mutation_faults(a,path):
    for variant in (0,4,6):
        h=fresh(a,path,512,variant,256)
        assert a.mkdir(b'/A')==0 and a.mkdir(b'/B')==0 and a.create(b'/A/file')==0
        assert a.write(b'/A/file',0,b'original',8)==0
        assert a.hardlink(b'/A',b'/Alias')==0
        assert a.unmount()==0;h.close();base=path.read_bytes()
        operations=(('grow',lambda:a.truncate(b'/A/file',1500)),
                    ('create-dir',lambda:a.mkdir(b'/A/new')),
                    ('rename',lambda:a.rename(b'/A/file',b'/B/moved')),
                    ('unlink',lambda:a.unlink(b'/A/file')),
                    ('link-directory',lambda:a.hardlink(b'/A',b'/B/Linked')),
                    ('promote-directory',lambda:a.unlink(b'/A')))
        for name,operation in operations:
            path.write_bytes(base);h=Host(path);assert a.mount(C.byref(h.io),None)==0
            assert operation()==0;writes=h.writes;barriers=h.flushes
            assert a.unmount()==0;h.close()
            for kind,total in (('write',writes),('flush',barriers)):
                for nth in range(1,total+1):
                    path.write_bytes(base);h=Host(path);assert a.mount(C.byref(h.io),None)==0
                    if kind=='write':h.fail_write=nth
                    else:h.fail_flush=nth
                    r=operation();assert r==-2,(variant,name,kind,nth,r)
                    h.fail_write=h.fail_flush=0
                    assert a.unmount()==0;h.close()
                    h=Host(path);h.io.flags=2;assert a.mount(C.byref(h.io),None)==0
                    c=Check();r=a.recover_bitmap(C.byref(c))
                    assert r in (0,-3),(variant,name,kind,nth,r,c.bad)
                    if r==0:a.check()
                    # Structural damage is reported, never guessed into a
                    # newly valid volume. Full rollback is not an OFS promise.
                    else:assert c.errors
                    assert a.unmount()==0;h.close()
            print('OFS-ALL-MUTATION-WRITE-BARRIER-FAULTS-PASS',variant,name,writes,barriers,flush=True)


def geometry(a,path):
    for bs in SIZES:
        for reserved in (1,2,3,17):
            h=fresh(a,path,bs,0,1025,reserved)
            s=space(a);assert s.root==(1025-1+reserved)//2
            assert a.create(b'/geometry')==0 and a.write(b'/geometry',0,b'G',1)==0
            a.check();assert a.unmount()==0
            o=Mount(C.sizeof(Mount),bs,reserved,0);assert a.mount(C.byref(h.io),C.byref(o))==0
            assert a.data(b'/geometry')==b'G';assert a.unmount()==0;h.close()
    print('OFS-ODD-GEOMETRY-RESERVED-ROOT-ALL-SIZES-PASS',flush=True)

def allocation_mount_safety(a,path):
    for variant in (0,2,4,6):
        h=fresh(a,path,512,variant,256)
        assert a.create(b'/victim')==0 and a.write(b'/victim',0,b'preserve',8)==0
        key=a.meta(b'/victim').block;root=space(a).root
        assert a.unmount()==0;h.close()
        bm=u32(block(path,root,512),316);b=block(path,bm,512)
        bit=key-2;at=4+4*(bit//32);put(b,at,u32(b,at)|(1<<(bit%32)))
        checksum(b,0);store(path,bm,512,b);before=digest(path)
        h=Host(path);assert a.mount(C.byref(h.io),None)==-3 and h.writes==0
        assert digest(path)==before
        h.io.flags=2;assert a.mount(C.byref(h.io),None)==0
        assert a.create(b'/new')==-6 and a.write(b'/victim',8,b'X',1)==-6
        assert digest(path)==before
        c=Check();assert a.recover_bitmap(C.byref(c))==0
        assert a.data(b'/victim')==b'preserve'
        assert a.write(b'/victim',8,b'X',1)==0 and a.create(b'/new')==0
        a.check();assert a.unmount()==0;h.close()
    print('OFS-WRITABLE-MOUNT-OWNERSHIP-AND-RECOVERY-GATE-PASS',flush=True)


def bitmap_extensions(a,path):
    h=fresh(a,path,512,0,1100000)
    root=space(a).root;rb=block(path,root,512);ext=u32(rb,416)
    assert ext and u32(block(path,ext,512),508)
    a.check();assert a.unmount()==0;h.close()
    saved=block(path,ext,512);bad=bytearray(saved);put(bad,508,ext);store(path,ext,512,bad)
    h=Host(path);assert a.mount(C.byref(h.io),None)==-3
    # Repeated verifier errors cannot retain a half-initialized bitmap index.
    h.io.flags=3;assert a.mount(C.byref(h.io),None)==0
    for _ in range(2):
        c=Check();assert a.verify(C.byref(c))==-3
    assert a.unmount()==0;h.close();store(path,ext,512,saved)
    h=Host(path);assert a.mount(C.byref(h.io),None)==0;a.check();assert a.unmount()==0;h.close()
    print('OFS-MULTIPLE-BITMAP-EXTENSIONS-CYCLE-PASS',flush=True)

def variant_corruption(a,path):
    for variant in (4,6):
        h=fresh(a,path,512,variant)
        p=b'/'+b'L'*106 if variant==6 else b'/entry'
        assert a.create(p)==0 and a.set_comment(p,b'c'*79)==0
        key=a.meta(p).block;root=space(a).root
        assert a.unmount()==0;h.close()
        if variant==4:
            cache=u32(block(path,root,512),504);raw=block(path,cache,512)
            for at,value in ((8,root+1),(12,100000),(16,cache)):
                b=bytearray(raw);put(b,at,value);checksum(b);store(path,cache,512,b)
                h=Host(path);assert a.mount(C.byref(h.io),None)==-3
                h.io.flags=3;assert a.mount(C.byref(h.io),None)==0
                before=h.writes;c=Check();assert a.verify(C.byref(c))==-3 and h.writes==before
                assert a.unmount()==0;h.close()
            store(path,cache,512,raw)
        else:
            rb=block(path,root,512);put(rb,468,1);checksum(rb);store(path,root,512,rb)
            h=Host(path);assert a.mount(C.byref(h.io),None)==-3
            h.io.flags=2;assert a.mount(C.byref(h.io),None)==0
            c=Check();assert a.recover_bitmap(C.byref(c))==0;a.check()
            assert a.unmount()==0;h.close()
            header=block(path,key,512);comment=u32(header,440)
            cb=block(path,comment,512);put(cb,8,root);checksum(cb);store(path,comment,512,cb)
            h=Host(path);h.io.flags=3;assert a.mount(C.byref(h.io),None)==0
            e=Entry();assert a.stat(p,C.byref(e))==-3
            c=Check();assert a.verify(C.byref(c))==-3
            assert a.unmount()==0;h.close()
    print('OFS-DOS4-CACHE-DOS6-COMMENT-COUNTER-NEGATIVE-PASS',flush=True)

def lnfs_full_promotion(a,path):
    h=fresh(a,path,512,6,32)
    assert a.create(b'/original')==0 and a.set_comment(b'/original',b'c'*79)==0
    alias=b'/'+b'A'*106
    assert a.hardlink(b'/original',alias)==0
    assert a.create(b'/fill')==0
    free=space(a).free//512;assert a.truncate(b'/fill',free*488)==0
    assert space(a).free==0
    assert a.unlink(b'/original')==0
    assert a.meta(alias).comment==b'c'*79
    assert a.set_comment(alias,b'd'*79)==0 # Reuses its allocated comment block.
    a.check();assert a.unmount()==0;h.close()
    print('OFS-LNFS-FULL-VOLUME-LINK-PROMOTION-COMMENT-REUSE-PASS',flush=True)


def unsigned_and_max_file(a):
    # A virtual read-only address fixture, not an allocated real-world volume.
    # Synthesized metadata exercises the real SO's uint32 keys/uint64 I/O at
    # the format limit without writing terabytes or claiming physical tests.
    for bs in SIZES:
        blocks=0xffffffff;root=(blocks+1)//2;filekey=0x80000010
        table=bs//4-56;bits=(bs-4)*8;pages=(blocks-2+bits-1)//bits
        per=bs//4-1;extbase=2+pages
        size=0xffffffff if bs==32768 else 19
        total=(size-1)//(bs-24)+1;lists=(total+table-1)//table
        data_base=0x90000000;list_base=0x81000000
        name=b'Maximum';h=len(name)
        for ch in name:h=(h*13+(ch-32 if 97<=ch<=122 else ch))&0x7ff
        bucket=h%table
        cached={}
        def get(key):
            if key in cached:return cached[key]
            b=bytearray(bs)
            if key==0:
                put(b,0,0x444f5300);put(b,8,root)
            elif key==root:
                put(b,0,2);put(b,12,table);put(b,24+bucket*4,filekey)
                put(b,bs-200,0xffffffff)
                for j in range(min(pages,25)):put(b,bs-196+j*4,2+j)
                if pages>25:put(b,bs-96,extbase)
                b[bs-80]=7;b[bs-79:bs-72]=b'ADDRESS';put(b,bs-4,1);checksum(b)
            elif extbase<=key<extbase+(max(pages-25,0)+per-1)//per:
                first=25+(key-extbase)*per
                for j in range(min(per,pages-first)):put(b,j*4,2+first+j)
                if first+per<pages:put(b,bs-4,key+1)
            elif key==filekey or list_base<=key<list_base+lists-1:
                index=0 if key==filekey else key-list_base+1
                base=index*table;count=min(table,total-base)
                put(b,0,2 if index==0 else 16);put(b,4,key);put(b,8,count)
                if index==0:
                    put(b,16,data_base);put(b,bs-188,size);put(b,bs-12,root)
                    b[bs-80]=len(name);b[bs-79:bs-79+len(name)]=name
                else:put(b,bs-12,filekey)
                for j in range(count):put(b,bs-204-j*4,data_base+base+j)
                if index+1<lists:put(b,bs-8,list_base+index)
                put(b,bs-4,0xfffffffd);checksum(b)
            elif data_base<=key<data_base+total:
                index=key-data_base;count=min(bs-24,size-index*(bs-24))
                put(b,0,8);put(b,4,filekey);put(b,8,index+1);put(b,12,count)
                if index+1<total:put(b,16,key+1)
                for j in range(count):b[24+j]=(index+j*7)&255
                checksum(b)
            # Bitmap pages contain zero words: all allocated. Only the
            # requested ownership/read boundary is qualified by this fixture.
            data=bytes(b)
            if key==root or key==filekey or key>=list_base:cached[key]=data
            return data
        calls=[0];highest=[0]
        def read(_,at,out,n):
            if at>blocks*bs or n>blocks*bs-at:return -1
            calls[0]+=1;highest[0]=max(highest[0],at+n)
            first=at//bs;last=(at+n+bs-1)//bs
            data=b''.join(get(k) for k in range(first,last));off=at%bs
            C.memmove(out,data[off:off+n],n);return 0
        cb=READ(read);io=Io();io.size=C.sizeof(Io);io.flags=1;io.bytes=blocks*bs;io.read_at=cb
        opt=Mount(C.sizeof(Mount),bs,2,0)
        r=a.mount(C.byref(io),C.byref(opt));assert r==0,('unsigned mount',bs,r)
        assert a.meta(b'/Maximum').bytes==size
        out=C.create_string_buffer(19);at=size-19
        assert a.read(b'/Maximum',at,out,19)==0
        logical=at//(bs-24);within=at%(bs-24)
        expect=bytes((logical+(within+j)*7)&255 for j in range(19))
        assert out.raw==expect,(bs,out.raw,expect)
        assert a.read(b'/Maximum',size,out,0)==0
        assert highest[0]>2**40
        assert a.unmount()==0
        print('OFS-UNSIGNED-HIGH-KEY-64BIT-OFFSET-PASS',bs,hex(filekey),size,calls[0],flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('--module',type=Path,required=True)
    p.add_argument('--quick',action='store_true');p.add_argument('--sanitized',action='store_true');args=p.parse_args()
    a=Api(args.module.resolve())
    with tempfile.TemporaryDirectory(prefix='ofs-complete-') as tmp:
        path=Path(tmp)/'regular.img'
        for bs in ((512,) if args.quick else SIZES):
            for variant in (0,2,4,6):
                metadata(a,path,bs,variant)
                directory_links(a,path,bs,variant)
        for bs in ((512,) if args.quick else (512,1024,4096)):extension(a,path,bs)
        for variant in (0,4,6):full_media(a,path,variant)
        for variant in ((0,) if args.quick else (0,4,6)):many_entries(a,path,variant,128 if args.sanitized else 600)
        corruption(a,path);faults(a,path);mutation_faults(a,path);geometry(a,path);unsigned_and_max_file(a)
        allocation_mount_safety(a,path);bitmap_extensions(a,path);variant_corruption(a,path);lnfs_full_promotion(a,path)
        full_directory_promotion(a,path)
    print('OFS-SO-COMPLETE-BOUNDARY-FAILURE-SUITE-PASS')

if __name__=='__main__':main()
