/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common filesystem interface over the canonical, single-volume OFS V3 engine.
 */
#include "ofs_v3.h"
#include "../core/ofs_engine.h"
#include <infiltratr/filesystem_module_v2.h>
extern OfsFs ifs_ofs_active;
static struct {
    IfsFsHostRuntimeV2 host;
    uintptr_t generation, handle;
    int mounted, writable;
} session;
static int eligible(const IfsFsHostRuntimeV2 *host) {
    return host && ifs_fs_host_v2_valid(host) &&
        host->volume_bytes >= 8192u;
}
static int live(void *v) {
    return session.mounted && session.handle &&
        (uintptr_t)v == session.handle && ifs_ofs_active.mounted;
}
static int can_write(void) { return session.writable && ifs_ofs_active.mounted; }
static IfsOfsIoV3 native(const IfsFsHostRuntimeV2 *h, int write) {
    IfsOfsIoV3 n = {0};
    n.structure_bytes = sizeof(n);
    n.flags = write ? 0u : IFS_OFS_READ_ONLY;
    n.context=h->context; n.volume_bytes=h->volume_bytes;
    n.read_at=h->read_at; n.write_at=write ? h->write_at : NULL;
    n.flush=write ? h->flush : NULL;
    n.allocate=h->allocate; n.release=h->release;
    return n;
}
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant) {
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
static int mount(const IfsFsHostRuntimeV2 *h, void **v, IfsFsVolumeInfoV2 *info) {
    const IfsOfsModuleV3 *m=infiltrator_filesystem_module_v3();
    IfsOfsSpaceV3 space={0};
    int r,write;
    if (!eligible(h) || !v || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *v=NULL;
    if (session.mounted || ifs_ofs_active.mounted ||
        session.generation == UINTPTR_MAX) return IFS_FS_BUSY;
    write=!(h->capabilities & IFS_FS_HOST_READ_ONLY) &&
        (h->capabilities & IFS_FS_HOST_EXCLUSIVE) &&
        (h->capabilities & IFS_FS_HOST_DURABLE) &&
        h->write_at && h->flush;
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
static int assign(const IfsOfsEntryV3 *e, IfsFsEntryV2 *out) {
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
static int stat_path(void *v, const char *p, IfsFsEntryV2 *out) {
    IfsOfsEntryV3 e={0};
    if(!live(v)||!p||!out)return IFS_FS_INVALID;
    e.structure_bytes=sizeof(e);
    int r=infiltrator_filesystem_module_v3()->stat(p,&e);
    return r?r:assign(&e,out);
}
static int nth(void *v,const char *p,uint32_t index,IfsFsEntryV2 *out){
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
static int format(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                  const char *label,uint32_t flags){
    IfsOfsFormatV3 options={0};
    if(!eligible(h))return IFS_FS_INVALID;
    if(flags)return IFS_FS_UNSUPPORTED;
    if((h->capabilities & IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities & IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities & IFS_FS_HOST_DURABLE) ||
       !h->write_at || !h->flush)return IFS_FS_ACCESS;
    if(session.mounted||ifs_ofs_active.mounted)return IFS_FS_BUSY;
    if(h->logical_sector_bytes<512u ||
       h->logical_sector_bytes>32768u ||
       (h->logical_sector_bytes&(h->logical_sector_bytes-1u)) ||
       (variant!=0u&&variant!=2u&&variant!=4u&&variant!=6u))
       return IFS_FS_INVALID;
    options.structure_bytes=sizeof(options);
    options.block_bytes=h->logical_sector_bytes;
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
/* Common V2 is the sole exported format-neutral contract. Its operations
 * use the authoritative OFS V3 engine and the verified V2 host lease.
 * Recovery/defrag remain unadvertised until translated to DEFRAG V2. */
static int volume_query(void *mounted,IfsFsVolumeInfoV2 *info) {
    IfsOfsSpaceV3 space={0};
    if(!live(mounted)||!info||info->structure_bytes<sizeof(*info))
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
static int volume_label(void *mounted, char *out, uint32_t capacity, uint32_t *bytes) {
    if (!live(mounted) || !out || !capacity || !bytes)
        return IFS_FS_INVALID;
    OfsNode root;
    OfsFs *fs = &ifs_ofs_active;
    const int status = ofs_node(fs, fs->root, &root, fs->scratch[1]);
    if (status != IFS_FS_OK)
        return status;

    uint32_t length = 0;
    for (uint32_t i = 0; i < root.name_bytes; ++i)
        length += root.name[i] >= 128u ? 2u : 1u;
    if (length >= capacity)
        return IFS_FS_NO_SPACE;

    const int converted = ofs_utf8(root.name, root.name_bytes, out, capacity);
    if (converted == IFS_FS_OK)
        *bytes = length;
    return converted;
}
static int volume_sync(void *mounted) {
    if(!live(mounted))return IFS_FS_INVALID;
    return infiltrator_filesystem_module_v3()->flush()==0?
        IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int volume_check(void *mounted,uint32_t flags,uint64_t *errors) {
    uint32_t count=0;
    if(flags)return IFS_FS_UNSUPPORTED;
    if(!errors)return IFS_FS_INVALID;
    const int r=check(mounted,&count);
    if(r==IFS_FS_OK)*errors=count;
    return r;
}
__attribute__((used,section(".ifsmeta")))
static const char identity_v2[]="IFS-MODULE:ofs:common2";
static const IfsFsCoreOpsV2 core_v2={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
       IFS_FS_GROUP_CORE,0u,
       IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
       IFS_FS_CORE_LIST|IFS_FS_CORE_READ|IFS_FS_CORE_WRITE|
       IFS_FS_CORE_CREATE|IFS_FS_CORE_REMOVE|IFS_FS_CORE_RENAME|
       IFS_FS_CORE_TRUNCATE},
    .probe=probe,.mount=mount,.unmount=unmount,
    .stat=stat_path,.directory_entry=nth,
    .read=read_file,.write=write_file,.create=create_path,
    .remove=remove_path,.rename=rename_path,.truncate=resize_path
};
static const IfsFsVolumeOpsV2 volume_v2={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
       IFS_FS_GROUP_VOLUME,0u,
       IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET|IFS_FS_VOLUME_SYNC|
       IFS_FS_VOLUME_CHECK|IFS_FS_VOLUME_FORMAT},
    .query=volume_query,.label=volume_label,.sync=volume_sync,
    .check=volume_check,.format=format
};
static const IfsFsGroupHeaderV2 *query_v2(
    uint32_t id,uint32_t low,uint32_t high){
    if(low>IFS_FS_GROUP_ABI_V2||high<IFS_FS_GROUP_ABI_V2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core_v2.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume_v2.header;
    return NULL;
}
static const IfsFsModuleApiV2 api_v2={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"ofs",
    IFS_FS_PROFILE_HOSTED|IFS_FS_PROFILE_FREESTANDING,
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),query_v2
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){
    return &api_v2;
}
