/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Adapted from Boot tests/so_module_test.c; production ELF loader unchanged. */
#define _GNU_SOURCE
#include "so_elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr,"SO test failed at line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static unsigned allocations, releases;
static void *allocate(void *u, size_t bytes) {
    (void)u;
    void *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (p==MAP_FAILED) return 0;
    ++allocations; return p;
}
static void release(void *u, void *p, size_t bytes) {
    (void)u; REQUIRE(munmap(p,bytes)==0); ++releases;
}
static int protect(void *u, void *p, size_t bytes, uint32_t flags) {
    (void)u;
    return mprotect(p,bytes,((flags&4u)?PROT_READ:0) |
                           ((flags&2u)?PROT_WRITE:0) |
                           ((flags&1u)?PROT_EXEC:0));
}
static void *read_file(const char *name, size_t *bytes) {
    FILE *f=fopen(name,"rb");
    struct stat st;
    REQUIRE(f && fstat(fileno(f),&st)==0 && st.st_size>0);
    *bytes=(size_t)st.st_size;
    void *p=malloc(*bytes);
    REQUIRE(p && fread(p,1,*bytes,f)==*bytes && fclose(f)==0);
    return p;
}
typedef struct { FILE *file; uint64_t bytes; unsigned reads; } Disk;
static int read_at(void *u, uint64_t offset, void *out, uint32_t bytes) {
    Disk *d=u;
    REQUIRE(offset<=d->bytes && bytes<=d->bytes-offset);
    ++d->reads;
    return fseeko(d->file,(off_t)offset,SEEK_SET)==0 &&
           fread(out,1,bytes,d->file)==bytes ? 0 : -1;
}
static void *allocate_host(void *u,size_t bytes) { (void)u; return malloc(bytes); }
static void release_host(void *u,void *p) { (void)u; free(p); }
int main(int argc, char **argv) {
    REQUIRE(argc==3 || argc==5);
    size_t length;
    void *data=read_file(argv[argc==3?2:1],&length);
    if (argc==3) {
        REQUIRE(strcmp(argv[1],"--validate")==0);
        int status=BootSoValidate(data,length);
        free(data); return status==0?0:2;
    }
    BootSoMemory memory={0,allocate,release,protect};
    BootSoImage image;
    REQUIRE(BootSoValidate(data,length)==0);
    REQUIRE(BootSoLoad(data,length,&memory,&image)==0);
    free(data);
    REQUIRE(strcmp(image.format_id,"ofs")==0);
    const IfsFsCoreOpsV2 *api=image.core;
    REQUIRE(api->header.abi_version==IFS_FS_GROUP_ABI_V2);
    Disk disk={fopen(argv[2],"rb"),0,0};
    struct stat st;
    REQUIRE(disk.file && fstat(fileno(disk.file),&st)==0);
    disk.bytes=(uint64_t)st.st_size;
    IfsFsHostRuntimeV2 io={.abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(io),
        .capabilities=IFS_FS_HOST_READ_ONLY,.volume_bytes=disk.bytes,
        .logical_sector_bytes=512,.physical_sector_bytes=512,.maximum_transfer_bytes=65536,
        .context=&disk,.read_at=read_at,.allocate=allocate_host,.release=release_host};
    IfsFsVolumeInfoV2 volume_info={.structure_bytes=sizeof(volume_info)};
    void *volume=NULL,*duplicate=NULL;
    REQUIRE(api->mount(&io,&volume,&volume_info)==0 && volume);
    REQUIRE(api->mount(&io,&duplicate,&volume_info)!=0 && !duplicate);
    REQUIRE(image.volume && image.volume->label);
    char label[64] = "#";
    uint32_t label_bytes = 0;
    REQUIRE(image.volume->label(volume,label,1,&label_bytes)==IFS_FS_NO_SPACE);
    REQUIRE(label[0]=='#' && label_bytes==0);
    REQUIRE(image.volume->label(volume,label,sizeof(label),&label_bytes)==IFS_FS_OK);
    REQUIRE(label_bytes==sizeof("SOFIXTURE")-1 && strcmp(label,"SOFIXTURE")==0);
    char name[1024];
    IfsFsEntryV2 info={.structure_bytes=sizeof(info),.name=name,.name_capacity=sizeof(name)};
    REQUIRE(api->stat(volume,"/missing-file",&info)==IFS_FS_NOT_FOUND);
    REQUIRE(api->stat(volume,"/",&info)==0 && info.type==IFS_FS_ENTRY_DIRECTORY);
    int found=0;
    for (uint32_t i=0;;++i) {
        int status=api->directory_entry(volume,"/",i,&info);
        if (status==IFS_FS_END) break;
        REQUIRE(status==0 && i<100);
        if (strcmp(info.name,"Nested Directory")==0) found=1;
    }
    REQUIRE(found);
    REQUIRE(api->stat(volume,argv[3],&info)==0 && info.type==IFS_FS_ENTRY_FILE);
    size_t expected_bytes;
    unsigned char *expected=read_file(argv[4],&expected_bytes);
    REQUIRE(info.size_bytes==expected_bytes);
    unsigned char buffer[16384];
    for (uint64_t position=0; position<expected_bytes;) {
        uint32_t requested=(expected_bytes-position)>sizeof(buffer)?sizeof(buffer):
                            (uint32_t)(expected_bytes-position);
        REQUIRE(api->read(volume,argv[3],position,buffer,requested)==0);
        REQUIRE(memcmp(buffer,expected+position,requested)==0);
        position+=requested;
    }
    REQUIRE(api->read(volume,argv[3],7,buffer,117)==0 &&
            memcmp(buffer,expected+7,117)==0);
    REQUIRE(api->unmount(volume)==0);
    REQUIRE(api->mount(&io,&volume,&volume_info)==0 && api->unmount(volume)==0);
    REQUIRE(fclose(disk.file)==0 && disk.reads>0);
    free(expected);
    BootSoUnload(&image);
    REQUIRE(allocations==releases);
    puts("BOOT-SAME-SO-READ-SEEK-DIRECTORY-PASS");
    return 0;
}
