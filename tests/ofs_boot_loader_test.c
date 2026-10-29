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
static int IFS_BOOT_CALL read_at(void *u, uint64_t offset, void *out, uint32_t bytes) {
    Disk *d=u;
    REQUIRE(offset<=d->bytes && bytes<=d->bytes-offset);
    ++d->reads;
    return fseeko(d->file,(off_t)offset,SEEK_SET)==0 &&
           fread(out,1,bytes,d->file)==bytes ? 0 : -1;
}
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
    REQUIRE(strcmp(image.manifest.format_id,"ofs")==0);
    const IfsBootModuleV1 *api=image.api;
    REQUIRE(api->abi_version==1 && api->structure_bytes==sizeof(*api));
    Disk disk={fopen(argv[2],"rb"),0,0};
    struct stat st;
    REQUIRE(disk.file && fstat(fileno(disk.file),&st)==0);
    disk.bytes=(uint64_t)st.st_size;
    IfsBootIo io={&disk,disk.bytes,512,sizeof(IfsBootIo),read_at};
    REQUIRE(api->mount(&io)==0 && api->mount(&io)!=0);
    void *file=calloc(1,api->file_context_bytes);
    void *directory=calloc(1,api->file_context_bytes);
    IfsBootInfo info;
    REQUIRE(file && directory);
    REQUIRE(api->open("/missing-file",file,&info)==IFS_BOOT_NOT_FOUND);
    REQUIRE(api->open("/",directory,&info)==0 && (info.attributes&IFS_BOOT_DIRECTORY));
    int found=0;
    for (uint32_t i=0;;++i) {
        int status=api->directory_entry(directory,i,&info);
        if (status==IFS_BOOT_END) break;
        REQUIRE(status==0 && i<100);
        if (strcmp(info.name,"Nested Directory")==0) found=1;
    }
    REQUIRE(found && api->close(directory)==0);
    REQUIRE(api->open(argv[3],file,&info)==0 && !(info.attributes&IFS_BOOT_DIRECTORY));
    size_t expected_bytes;
    unsigned char *expected=read_file(argv[4],&expected_bytes);
    REQUIRE(info.bytes==expected_bytes && api->unmount()!=0);
    unsigned char buffer[16384];
    for (uint64_t position=0; position<expected_bytes;) {
        uint32_t requested=(expected_bytes-position)>sizeof(buffer)?sizeof(buffer):
                            (uint32_t)(expected_bytes-position);
        uint32_t got=0;
        REQUIRE(api->read(file,position,buffer,requested,&got)==0 && got==requested);
        REQUIRE(memcmp(buffer,expected+position,got)==0);
        position+=got;
    }
    uint32_t got=99;
    REQUIRE(api->read(file,info.bytes,buffer,sizeof(buffer),&got)==0 && got==0);
    REQUIRE(api->read(file,7,buffer,117,&got)==0 && got==117 &&
            memcmp(buffer,expected+7,117)==0);
    REQUIRE(api->read(file,info.bytes+1u,buffer,1,&got)!=0 && got==0);
    REQUIRE(api->close(file)==0 && api->unmount()==0);
    REQUIRE(api->mount(&io)==0 && api->unmount()==0);
    REQUIRE(fclose(disk.file)==0 && disk.reads>0);
    free(file); free(directory); free(expected);
    BootSoUnload(&image);
    REQUIRE(allocations==releases);
    puts("BOOT-SAME-SO-READ-SEEK-DIRECTORY-PASS");
    return 0;
}
