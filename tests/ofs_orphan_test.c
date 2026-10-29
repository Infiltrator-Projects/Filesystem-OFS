// SPDX-License-Identifier: GPL-3.0-or-later
/* Canonical deferred-deletion durability and every writer/flush failure. */
#include "ofs_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char *data; size_t bytes; unsigned calls, fail, allocations; } Host;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"orphan test line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static int rd(void *ctx, uint64_t at, void *out, uint32_t n) {
    Host *h=ctx; if (at>h->bytes || n>h->bytes-at) return -1;
    memcpy(out,h->data+at,n); return 0;
}
static int wr(void *ctx, uint64_t at, const void *in, uint32_t n) {
    Host *h=ctx; if (++h->calls==h->fail || at>h->bytes || n>h->bytes-at) return -1;
    memcpy(h->data+at,in,n); return 0;
}
static int flush(void *ctx) { Host *h=ctx; return ++h->calls==h->fail ? -1:0; }
static void *alloc(void *ctx, size_t n) { Host *h=ctx; void *p=malloc(n); if(p)++h->allocations; return p; }
static void release(void *ctx, void *p) { Host *h=ctx; if(p){CHECK(h->allocations);--h->allocations;free(p);} }
static int clock_io(void *ctx, IfsOfsDate *out) { (void)ctx; *out=(IfsOfsDate){123,45,67}; return 0; }
static void mount_image(OfsFs *f, Host *h) {
    IfsOfsIoV3 io={sizeof(io),0,h,h->bytes,rd,wr,flush,alloc,release,clock_io};
    memset(f,0,sizeof(*f)); CHECK(ofs_probe(f,&io,NULL)==0); h->calls=0;
}
static uint32_t lookup(OfsFs *f,const char *path) {
    OfsNode n; CHECK(ofs_path(f,path,0,&n)==0); return n.object;
}
static void verify(OfsFs *f) { IfsOfsCheckV3 c; CHECK(ofs_verify(f,&c,0)==0); }
static void destroy(OfsFs *f,Host *h) { ofs_release(f);CHECK(h->allocations==0);free(f); }
static uint32_t root_flag(OfsFs *f,Host *h) { return ofs_be32(h->data+(size_t)f->root*f->block_bytes+f->block_bytes-200); }
static void success(Host *h, const unsigned char *original, uint32_t bs) {
    memcpy(h->data,original,h->bytes); h->fail=0;
    OfsFs *f=calloc(1,sizeof(*f)); CHECK(f); mount_image(f,h);
    CHECK(ofs_create(f,"/D",IFS_OFS_DIRECTORY,NULL,0)==0);
    CHECK(ofs_create(f,"/S",IFS_OFS_SOFTLINK,"F",0)==0);
    uint32_t file=lookup(f,"/F"),dir=lookup(f,"/D"),link=lookup(f,"/S");
    CHECK(ofs_unlink_deferred(f,"/F")==0);CHECK(root_flag(f,h)==0);verify(f);
    CHECK(ofs_unlink_deferred(f,"/D")==0);CHECK(root_flag(f,h)==0);verify(f);
    CHECK(ofs_unlink_deferred(f,"/S")==0);CHECK(root_flag(f,h)==0);verify(f);
    OfsNode n; CHECK(ofs_node(f,file,&n,f->scratch[0])==0);
    CHECK(ofs_write_checked(f,&n,bs+7,"kept",4)==0);
    CHECK(ofs_set_metadata_key(f,file,3,"open unlinked file")==0);verify(f);
    CHECK(root_flag(f,h)==0);
    CHECK(ofs_create(f,"/F",IFS_OFS_FILE,NULL,0)==0);CHECK(lookup(f,"/F")!=file);
    CHECK(ofs_create(f,"/Source",IFS_OFS_FILE,NULL,0)==0);
    CHECK(ofs_rename_deferred(f,"/Source","/F")==0);verify(f);
    CHECK(ofs_release_orphan(f,dir)==0);CHECK(root_flag(f,h)==0);verify(f);
    CHECK(ofs_release_orphan(f,link)==0);CHECK(root_flag(f,h)==0);verify(f);
    CHECK(ofs_release_orphan(f,file)==0);CHECK(root_flag(f,h)==0);verify(f);
    while(f->orphans) CHECK(ofs_release_orphan(f,f->orphans->key)==0);
    CHECK(root_flag(f,h)==UINT32_MAX);verify(f);
    destroy(f,h);
    f=calloc(1,sizeof(*f));CHECK(f);mount_image(f,h);verify(f);destroy(f,h);
}
static void failures(Host *h,const unsigned char *original,int closing) {
    unsigned barriers=0;
    for(unsigned fault=0;fault<=barriers;++fault) {
        memcpy(h->data,original,h->bytes);h->fail=0;
        OfsFs *f=calloc(1,sizeof(*f));CHECK(f);mount_image(f,h);
        uint32_t key=lookup(f,"/F");
        if(closing)CHECK(ofs_unlink_deferred(f,"/F")==0);
        h->calls=0;h->fail=fault;
        int r=closing?ofs_release_orphan(f,key):ofs_unlink_deferred(f,"/F");
        if(!fault){CHECK(r==0);barriers=h->calls;CHECK(barriers>0);}
        else {CHECK(r!=0);CHECK(!f->bitmap_valid || f->failed);}
        h->fail=0;destroy(f,h);
    }
    printf("OFS-ORPHAN-%s-EVERY-BARRIER-PASS %u\n",closing?"RELEASE":"DETACH",barriers);
}
int main(void) {
    for(uint32_t bs=512;bs<=32768;bs*=2)for(unsigned variant=0;variant<=6;variant+=2) {
        Host h={0};h.bytes=(size_t)bs*256;h.data=calloc(1,h.bytes);CHECK(h.data);
        OfsFs *f=calloc(1,sizeof(*f));CHECK(f);
        IfsOfsIoV3 io={sizeof(io),0,&h,h.bytes,rd,wr,flush,alloc,release,clock_io};
        IfsOfsFormatV3 format={sizeof(format),bs,2,variant,"ORPHANS"};
        CHECK(ofs_format(f,&io,&format)==0);mount_image(f,&h);
        CHECK(ofs_create(f,"/F",IFS_OFS_FILE,NULL,0)==0);
        OfsNode n;CHECK(ofs_path(f,"/F",1,&n)==0);
        CHECK(ofs_write_checked(f,&n,0,"preserved",9)==0);verify(f);destroy(f,&h);
        unsigned char *original=malloc(h.bytes);CHECK(original);memcpy(original,h.data,h.bytes);
        success(&h,original,bs);failures(&h,original,0);failures(&h,original,1);
        free(original);free(h.data);printf("OFS-ORPHAN-PROFILE-PASS %u %u\n",bs,variant);
    }
    puts("OFS-CANONICAL-OPEN-UNLINK-RENAME-FAILURE-PASS");return 0;
}
