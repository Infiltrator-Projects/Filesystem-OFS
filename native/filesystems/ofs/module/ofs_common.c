/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common filesystem interface over the canonical, single-volume OFS V3 engine.
 */
#include "ofs_v3.h"
#include "../core/ofs_engine.h"
#include <infiltratr/filesystem_module.h>
#include <infiltratr/filesystem_module_v2.h>
extern OfsFs ifs_ofs_active;
static struct {
    IfsFsHostIoV1 host;
    uintptr_t generation, handle;
    int mounted, writable;
} session;
static int eligible(const IfsFsHostIoV1 *host) {
    return host && host->structure_bytes >= sizeof(*host) &&
        host->read_at && host->volume_bytes >= 8192u &&
        !(host->flags & ~(IFS_FS_IO_READ_ONLY | IFS_FS_IO_EXCLUSIVE));
}
static int live(void *v) {
    return session.mounted && session.handle &&
        (uintptr_t)v == session.handle && ifs_ofs_active.mounted;
}
static int can_write(void) { return session.writable && ifs_ofs_active.mounted; }
static IfsOfsIoV3 native(const IfsFsHostIoV1 *h, int write) {
    IfsOfsIoV3 n = {0};
    n.structure_bytes = sizeof(n);
    n.flags = write ? 0u : IFS_OFS_READ_ONLY;
    n.context=h->context; n.volume_bytes=h->volume_bytes;
    n.read_at=h->read_at; n.write_at=write ? h->write_at : NULL;
    n.flush=write ? h->flush : NULL;
    n.allocate=h->allocate; n.release=h->release;
    return n;
}
static int probe(const IfsFsHostIoV1 *h, uint32_t *variant) {
    const IfsOfsModuleV3 *m = infiltrator_filesystem_module_v3();
    IfsOfsSpaceV3 space = {0};
    int r, closed;
    if (!eligible(h) || !variant) return IFS_FS_INVALID;
    if (session.mounted || ifs_ofs_active.mounted) return IFS_FS_BUSY;
    IfsOfsIoV3 io=native(h,0);
    r=m->mount(&io,NULL);
    if (r) return r == IFS_OFS_INVALID ? IFS_FS_NOT_FOUND : r;
    space.structure_bytes=sizeof(space);
    r=m->space(&space);
    closed=m->unmount();
    if (r || closed) return r ? r : closed;
    *variant=space.variant;
    return IFS_FS_OK;
}
static int mount(const IfsFsHostIoV1 *h, void **v, IfsFsVolumeInfoV1 *info) {
    const IfsOfsModuleV3 *m=infiltrator_filesystem_module_v3();
    IfsOfsSpaceV3 space={0};
    int r,write;
    if (!eligible(h) || !v || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *v=NULL;
    if (session.mounted || ifs_ofs_active.mounted ||
        session.generation == UINTPTR_MAX) return IFS_FS_BUSY;
    write=!(h->flags & IFS_FS_IO_READ_ONLY) &&
        (h->flags & IFS_FS_IO_EXCLUSIVE) && h->write_at &&
        h->flush && h->allocate && h->release;
    session.host=*h;
    IfsOfsIoV3 io=native(&session.host,write);
    r=m->mount(&io,NULL);
    if(r) return r;
    space.structure_bytes=sizeof(space);
    r=m->space(&space);
    if(r) { (void)m->unmount(); return r; }
    session.mounted=1;session.writable=write;
    session.handle=++session.generation;
    info->variant=space.variant;
    info->block_bytes=space.block_bytes;
    info->max_name_bytes=space.variant==6u?106u:30u;
    info->total_bytes=space.total_bytes;info->free_bytes=space.free_bytes;
    *v=(void *)session.handle;
    return IFS_FS_OK;
}
static int unmount(void *v) {
    if(!live(v))return IFS_FS_INVALID;
    int r=infiltrator_filesystem_module_v3()->unmount();
    if(!r){session.mounted=0;session.writable=0;session.handle=0;}
    return r;
}
static int assign(const IfsOfsEntryV3 *e, IfsFsEntryV1 *out) {
    uint32_t len=0;
    if(!out || out->structure_bytes<sizeof(*out) ||
       !out->name || !out->name_capacity) return IFS_FS_INVALID;
    while(len<sizeof(e->name) && e->name[len]) ++len;
    if(len==sizeof(e->name) || len>=out->name_capacity) return IFS_FS_NO_SPACE;
    for(uint32_t i=0;i<=len;++i) out->name[i]=e->name[i];
    out->name_bytes=len;out->size_bytes=e->bytes;out->attributes=e->protection;
    out->type=e->type==IFS_OFS_SOFTLINK?IFS_FS_ENTRY_LINK:
        (e->type==IFS_OFS_DIRECTORY || e->type==IFS_OFS_ROOT ||
         e->type==IFS_OFS_LINKDIR)?IFS_FS_ENTRY_DIRECTORY:IFS_FS_ENTRY_FILE;
    return IFS_FS_OK;
}
static int stat_path(void *v, const char *p, IfsFsEntryV1 *out) {
    IfsOfsEntryV3 e={0};
    if(!live(v)||!p||!out)return IFS_FS_INVALID;
    e.structure_bytes=sizeof(e);
    int r=infiltrator_filesystem_module_v3()->stat(p,&e);
    return r?r:assign(&e,out);
}
static int nth(void *v,const char *p,uint32_t index,IfsFsEntryV1 *out){
    IfsOfsEntryV3 e={0};
    if(!live(v)||!p||!out)return IFS_FS_INVALID;
    e.structure_bytes=sizeof(e);
    int r=infiltrator_filesystem_module_v3()->directory_entry(p,index,&e);
    return r?r:assign(&e,out);
}
static int read_file(void *v,const char *p,uint64_t at,void *b,uint32_t n){
    if(!live(v)||!p||(!b&&n))return IFS_FS_INVALID;
    if(at>UINT32_MAX||n>UINT32_MAX-at)return IFS_FS_UNSUPPORTED;
    return infiltrator_filesystem_module_v3()->read(p,(uint32_t)at,b,n);
}
static int write_file(void *v,const char *p,uint64_t at,const void *b,uint32_t n){
    if(!live(v)||!p||(!b&&n))return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    if(at>UINT32_MAX||n>UINT32_MAX-at)return IFS_FS_UNSUPPORTED;
    return infiltrator_filesystem_module_v3()->write(p,(uint32_t)at,b,n);
}
static int create_path(void *v,const char *p,uint32_t type){
    const IfsOfsModuleV3 *m=infiltrator_filesystem_module_v3();
    if(!live(v)||!p)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    if(type==IFS_FS_ENTRY_FILE)return m->create(p);
    if(type==IFS_FS_ENTRY_DIRECTORY)return m->mkdir(p);
    return IFS_FS_UNSUPPORTED;
}
static int remove_path(void *v,const char *p) {
    if(!live(v)||!p)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    return infiltrator_filesystem_module_v3()->unlink(p);
}
static int rename_path(void *v,const char *a,const char *b){
    if(!live(v)||!a||!b)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    return infiltrator_filesystem_module_v3()->rename(a,b);
}
static int resize_path(void *v,const char *p,uint64_t bytes){
    if(!live(v)||!p)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    if(bytes>UINT32_MAX)return IFS_FS_UNSUPPORTED;
    return infiltrator_filesystem_module_v3()->truncate(p,(uint32_t)bytes);
}
static int format(const IfsFsHostIoV1 *h,uint32_t variant,const char *label){
    const IfsFsHostFormatIoV1 *ext;
    IfsOfsFormatV3 options={0};
    if(!eligible(h))return IFS_FS_INVALID;
    if((h->flags & IFS_FS_IO_READ_ONLY) ||
       !(h->flags & IFS_FS_IO_EXCLUSIVE) || !h->write_at ||
       !h->flush || !h->allocate || !h->release)return IFS_FS_ACCESS;
    if(h->structure_bytes<sizeof(IfsFsHostFormatIoV1))return IFS_FS_UNSUPPORTED;
    if(session.mounted||ifs_ofs_active.mounted)return IFS_FS_BUSY;
    ext=(const IfsFsHostFormatIoV1 *)(const void *)h;
    if(ext->reserved||ext->logical_sector_bytes<512u ||
       ext->logical_sector_bytes>32768u ||
       (ext->logical_sector_bytes&(ext->logical_sector_bytes-1u)) ||
       (variant!=0u&&variant!=2u&&variant!=4u&&variant!=6u))
       return IFS_FS_INVALID;
    options.structure_bytes=sizeof(options);
    options.block_bytes=ext->logical_sector_bytes;
    options.variant=variant;options.label=label;
    IfsOfsIoV3 io=native(h,1);
    return infiltrator_filesystem_module_v3()->format(&io,&options);
}
static int check(void *v,uint32_t *errors){
    IfsOfsCheckV3 report={0};
    if(!live(v)||!errors)return IFS_FS_INVALID;
    report.structure_bytes=sizeof(report);
    int r=infiltrator_filesystem_module_v3()->verify(&report);
    *errors=report.errors;
    return r;
}
/* Recovery and defrag counts need separately qualified Common contracts. */
__attribute__((used,section(".ifsmeta")))
static const char common_identity[]="IFS-MODULE:ofs:common1";
static const IfsFsModuleApiV1 api={
    IFS_FS_COMMON_ABI_V1,sizeof(IfsFsModuleApiV1),"ofs",
    IFS_FS_CAP_PROBE|IFS_FS_CAP_MOUNT|IFS_FS_CAP_LIST|IFS_FS_CAP_READ|
    IFS_FS_CAP_WRITE|IFS_FS_CAP_CREATE|IFS_FS_CAP_REMOVE|IFS_FS_CAP_RENAME|
    IFS_FS_CAP_TRUNCATE|IFS_FS_CAP_FORMAT|IFS_FS_CAP_CHECK,
    probe,mount,unmount,stat_path,nth,read_file,write_file,create_path,
    remove_path,rename_path,resize_path,format,check,NULL
};
__attribute__((visibility("default")))
const IfsFsModuleApiV1 *infiltrator_filesystem_api_v1(void){return &api;}

/* V2 volume operations use the same serialized OFS engine as V1. */
static int v2_query(void *volume,IfsFsVolumeInfoV1 *info) {
    IfsOfsSpaceV3 space={0};
    if(!live(volume)||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    space.structure_bytes=sizeof(space);
    int r=infiltrator_filesystem_module_v3()->space(&space);
    if(r!=IFS_FS_OK)return r;
    info->variant=space.variant;
    info->block_bytes=space.block_bytes;
    info->max_name_bytes=space.variant==6u?106u:30u;
    info->total_bytes=space.total_bytes;
    info->free_bytes=space.free_bytes;
    return IFS_FS_OK;
}
static int v2_sync(void *volume) {
    if(!live(volume))return IFS_FS_INVALID;
    return infiltrator_filesystem_module_v3()->flush()==0?
        IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int v2_check(void *volume,uint32_t flags,uint64_t *errors) {
    uint32_t count=0;
    if(flags)return IFS_FS_UNSUPPORTED;
    if(!errors)return IFS_FS_INVALID;
    int r=check(volume,&count);
    if(r==IFS_FS_OK)*errors=count;
    return r;
}
static int v2_format(const IfsFsHostRuntimeV2 *host,
                     uint32_t variant,const char *label,uint32_t flags){
    IfsFsHostFormatIoV1 io={0};
    if(flags)return IFS_FS_UNSUPPORTED;
    if(!host)return IFS_FS_INVALID;
    if(host->capabilities & IFS_FS_HOST_READ_ONLY)return IFS_FS_ACCESS;
    int r=ifs_fs_host_v2_as_format_v1(host,&io);
    return r==IFS_FS_OK?format(&io.host,variant,label):r;
}
__attribute__((used,section(".ifsmeta")))
static const char identity_v2[]="IFS-MODULE:ofs:common2";
static const IfsFsVolumeOpsV1 volume_v2={
    .header={IFS_FS_GROUP_ABI_V1,sizeof(IfsFsVolumeOpsV1),
       IFS_FS_GROUP_VOLUME,0u,
       IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_SYNC|IFS_FS_VOLUME_CHECK|
       IFS_FS_VOLUME_FORMAT},
    .query=v2_query,.sync=v2_sync,.check=v2_check,.format=v2_format
};
static const IfsFsGroupHeaderV2 *query_v2(
    uint32_t id,uint32_t low,uint32_t high){
    if(id!=IFS_FS_GROUP_VOLUME||low>IFS_FS_GROUP_ABI_V1||
       high<IFS_FS_GROUP_ABI_V1)return NULL;
    return &volume_v2.header;
}
static const IfsFsModuleApiV2 api_v2={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"ofs",
    IFS_FS_PROFILE_HOSTED|IFS_FS_PROFILE_FREESTANDING,
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),&api,query_v2
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){
    return &api_v2;
}
