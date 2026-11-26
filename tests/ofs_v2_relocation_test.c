/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native Common V2 relocation on disposable memory media, with fresh
 * independent fixtures for every interrupted publication boundary. */
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BYTES (8u*1024u*1024u)
#define CHUNK (488u*80u)
typedef struct {uint8_t *data;unsigned writes,flushes,fail;} Media;
static int rd(void *ctx,uint64_t at,void *out,uint32_t n)
{
    Media *m=ctx;
    if(at%512u || n%512u || at>BYTES || n>BYTES-at)return -1;
    memcpy(out,m->data+at,n);return 0;
}
static int wr(void *ctx,uint64_t at,const void *in,uint32_t n)
{
    Media *m=ctx;
    if(at%512u || n%512u || at>BYTES || n>BYTES-at)return -1;
    memcpy(m->data+at,in,n);++m->writes;return 0;
}
static int fl(void *ctx){Media *m=ctx;return ++m->flushes==m->fail?-1:0;}
static void *alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void rel(void *ctx,void *p){(void)ctx;free(p);}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"OFS relocation line %d policy %u barrier %u: %s\n",__LINE__,policy,barrier,#x);goto done;}}while(0)
static int test(const IfsFsCoreOpsV2 *core,const IfsFsVolumeOpsV2 *vol,
                const IfsFsDefragOpsV2 *defrag,uint32_t policy,unsigned barrier)
{
    Media m={.data=calloc(1,BYTES)};
    if(!m.data)return 1;
    int result=1;
    IfsFsHostRuntimeV2 h={.abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=BYTES,.logical_sector_bytes=512u,.physical_sector_bytes=512u,
        .maximum_transfer_bytes=65536u,.context=&m,.read_at=rd,.write_at=wr,
        .flush=fl,.allocate=alloc,.release=rel},ro=h;
    ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *session=NULL,*plan=NULL;
    uint8_t a[CHUNK],b[CHUNK],copy[CHUNK];
    for(unsigned i=0u;i<CHUNK;++i){a[i]=(uint8_t)(i*13u+17u);b[i]=(uint8_t)(i*19u+23u);}
    CHECK(vol->format(&h,0u,"COWTEST",0u)==IFS_FS_OK);
    CHECK(core->mount(&h,&session,&info)==IFS_FS_OK);
    CHECK(core->create(session,"/A",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(session,"/B",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    for(unsigned i=0u;i<3u;++i) {
        CHECK(core->write(session,"/A",(uint64_t)i*CHUNK,a,CHUNK)==IFS_FS_OK);
        CHECK(core->write(session,"/B",(uint64_t)i*CHUNK,b,CHUNK)==IFS_FS_OK);
    }
    CHECK(core->unmount(session)==IFS_FS_OK);session=NULL;
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK);
    IfsFsExtentV2 ex[2];uint32_t n=0u;
    CHECK(defrag->map(session,"/A",79u,ex,2u,&n)==IFS_FS_OK && n==2u);
    CHECK(ex[1].physical_offset!=ex[0].physical_offset+512u);
    unsigned before=m.writes;
    CHECK(defrag->plan(session,"/",policy,&plan)==IFS_FS_OK && plan);
    uint64_t moved=99u;
    CHECK(defrag->execute(session,plan,&moved)==IFS_FS_ACCESS && !moved);
    defrag->release_plan(session,plan);plan=NULL;
    CHECK(m.writes==before);
    CHECK(core->unmount(session)==IFS_FS_OK);session=NULL;
    CHECK(core->mount(&h,&session,&info)==IFS_FS_OK);
    CHECK(defrag->plan(session,"/",policy,&plan)==IFS_FS_OK && plan);
    if(barrier) {
        m.fail=m.flushes+barrier;
        CHECK(defrag->execute(session,plan,&moved)==IFS_FS_IO_ERROR);
        defrag->release_plan(session,plan);plan=NULL;
        m.fail=0u;
        CHECK(core->unmount(session)==IFS_FS_OK);session=NULL;
        uint32_t variant=99u;
        CHECK(core->probe(&ro,&variant)==IFS_FS_OK && variant==0u);
        CHECK(core->mount(&ro,&session,&info)!=IFS_FS_OK && !session);
        CHECK(core->mount(&h,&session,&info)==IFS_FS_OK && session);
        uint8_t byte=0;
        CHECK(core->read(session,"/A",0u,&byte,1u)!=IFS_FS_OK);
        uint64_t repaired=0u;
        CHECK(defrag->recover(session,0u,&repaired)==IFS_FS_OK && repaired);
        for(unsigned i=0u;i<3u;++i) {
            CHECK(core->read(session,"/A",(uint64_t)i*CHUNK,copy,CHUNK)==IFS_FS_OK &&
                  !memcmp(copy,a,CHUNK));
            CHECK(core->read(session,"/B",(uint64_t)i*CHUNK,copy,CHUNK)==IFS_FS_OK &&
                  !memcmp(copy,b,CHUNK));
        }
        CHECK(defrag->plan(session,"/",policy,&plan)==IFS_FS_OK && plan);
    }
    CHECK(defrag->execute(session,plan,&moved)==IFS_FS_OK && moved);
    CHECK(defrag->verify(session,plan)==IFS_FS_OK);
    defrag->release_plan(session,plan);plan=NULL;
    CHECK(core->unmount(session)==IFS_FS_OK);session=NULL;
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK);
    for(unsigned i=0u;i<3u;++i) {
        CHECK(core->read(session,"/A",(uint64_t)i*CHUNK,copy,CHUNK)==IFS_FS_OK &&
              !memcmp(copy,a,CHUNK));
        CHECK(core->read(session,"/B",(uint64_t)i*CHUNK,copy,CHUNK)==IFS_FS_OK &&
              !memcmp(copy,b,CHUNK));
    }
    CHECK(defrag->map(session,"/A",79u,ex,2u,&n)==IFS_FS_OK && n==2u);
    CHECK(ex[1].physical_offset==ex[0].physical_offset+512u);
    uint64_t errors=99u;
    CHECK(vol->check(session,0u,&errors)==IFS_FS_OK && !errors);
    CHECK(core->unmount(session)==IFS_FS_OK);session=NULL;
    result=0;
done:
    if(plan && session)defrag->release_plan(session,plan);
    if(session)(void)core->unmount(session);
    free(m.data);return result;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!so)return 1;
    void *symbol=dlsym(so,IFS_FS_COMMON_EXPORT_V2);
    IfsFsModuleApiEntryV2 entry=NULL;
    if(!symbol || sizeof(entry)!=sizeof(symbol))return 1;
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry();
    if(!ifs_fs_api_v2_valid(api,"ofs"))return 1;
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *defrag=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    if(!core || !vol || !defrag || defrag->header.capabilities!=IFS_FS_DEFRAG_ALL)return 1;
    for(uint32_t policy=0u;policy<=10u;policy+=10u)
        for(unsigned barrier=0u;barrier<=4u;++barrier)
            if(test(core,vol,defrag,policy,barrier))return 1;
    puts("OFS Common V2 real relocation, growth and four durable failure boundaries PASS");
    return dlclose(so)!=0;
}
