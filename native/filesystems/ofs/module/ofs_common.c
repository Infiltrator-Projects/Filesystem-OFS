/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V2 interface over the canonical OFS native format engine.
 */
#include "ofs_v3.h"
#include "../core/ofs_engine.h"
#include <infiltratr/filesystem_module_v2.h>
#include <string.h>
#include <stdatomic.h>

/* The native engine has one active image and shared scratch/IO state.
 * Enforce exclusion inside the V2 provider, not merely in its callers.
 * A reentrant host callback must receive BUSY rather than deadlocking
 * or replacing the device while a transfer is in progress. */
static atomic_flag v2_operation_gate=ATOMIC_FLAG_INIT;
static int v2_enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&v2_operation_gate,memory_order_acquire);
}
static int v2_leave_operation(int rc)
{
    atomic_flag_clear_explicit(&v2_operation_gate,memory_order_release);
    return rc;
}

OfsFs ifs_ofs_active;
static struct {
    IfsFsHostRuntimeV2 host;
    uintptr_t generation, handle;
    int mounted, writable, allocation_verified;
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
/* The OFS engine uses byte-granular requests, but V2 host callbacks accept
 * only aligned sectors. Preserve the host lease while adapting those requests;
 * writes use bounded read-modify-write under its exclusive durable lease. */
static int host_read(void *context,uint64_t at,void *data,uint32_t bytes) {
    return ifs_fs_host_read_bytes_v2(context,at,data,bytes)==IFS_FS_OK ? 0 : -1;
}
static int host_write(void *context,uint64_t at,const void *data,uint32_t bytes) {
    return ifs_fs_host_write_bytes_v2(context,at,data,bytes)==IFS_FS_OK ? 0 : -1;
}
static int host_flush(void *context) {
    const IfsFsHostRuntimeV2 *h=context;
    return h->flush(h->context);
}
static void *host_allocate(void *context,size_t bytes) {
    const IfsFsHostRuntimeV2 *h=context;
    return h->allocate(h->context,bytes);
}
static void host_release(void *context,void *data) {
    const IfsFsHostRuntimeV2 *h=context;
    h->release(h->context,data);
}
static IfsOfsIoV3 native(const IfsFsHostRuntimeV2 *h, int write) {
    IfsOfsIoV3 n = {0};
    n.structure_bytes = sizeof(n);
    n.flags = write ? 0u : IFS_OFS_READ_ONLY;
    n.context=(void *)h; n.volume_bytes=h->volume_bytes;
    n.read_at=host_read; n.write_at=write ? host_write : NULL;
    n.flush=write ? host_flush : NULL;
    n.allocate=host_allocate; n.release=host_release;
    return n;
}

/* The V2 provider executes the canonical format core directly. The retired
 * V3 dispatch table is not linked into the production filesystem module. */
static int native_unmount(void) {
    OfsFs *f = &ifs_ofs_active;
    if (!f->mounted) return IFS_OFS_INVALID;
    int rc = (f->io.flags & IFS_OFS_ORDERED_WRITEBACK) ? ofs_sync_pending(f) :
        (f->io.flags & IFS_OFS_READ_ONLY) ? IFS_OFS_OK :
        f->io.flush(f->io.context) == 0 ? IFS_OFS_OK : IFS_OFS_IO_ERROR;
    ofs_release(f);
    return rc;
}
static int native_stat(const char *path, IfsOfsEntryV3 *out) {
    OfsNode node;
    int rc = ofs_path(&ifs_ofs_active, path, 1, &node);
    return rc ? rc : ofs_entry(&ifs_ofs_active, &node, out);
}
static int native_nth(const char *path, uint32_t index, IfsOfsEntryV3 *out) {
    OfsFs *fs = &ifs_ofs_active;
    OfsNode directory, child;
    int rc = ofs_path(fs, path, 1, &directory);
    if (rc) return rc;
    if (directory.object_type != IFS_OFS_ROOT &&
        directory.object_type != IFS_OFS_DIRECTORY) return IFS_OFS_INVALID;
    OfsDirCursor cursor = {0};
    uint64_t first = 0;
    if (fs->enumeration_valid &&
        fs->enumeration_directory == directory.object &&
        index >= fs->enumeration_next_index) {
        cursor = fs->enumeration_cursor;
        first = fs->enumeration_next_index;
    }
    for (uint64_t i = first; i <= (uint64_t)index; ++i) {
        rc = ofs_next(fs, directory.object, &cursor, &child);
        if (rc) {
            fs->enumeration_valid = 0;
            return rc;
        }
    }
    rc = ofs_entry(fs, &child, out);
    if (rc || index == UINT32_MAX) {
        fs->enumeration_valid = 0;
        return rc;
    }
    fs->enumeration_cursor = cursor;
    fs->enumeration_directory = directory.object;
    fs->enumeration_next_index = index + 1u;
    fs->enumeration_valid = 1;
    return IFS_OFS_OK;
}
static int native_read(const char *path, uint32_t offset, void *bytes, uint32_t count) {
    OfsNode file;
    int rc = ofs_path(&ifs_ofs_active, path, 1, &file);
    return rc ? rc : ofs_read_cached(&ifs_ofs_active, &file, offset, bytes, count);
}
static int native_write(const char *path, uint32_t offset, const void *bytes, uint32_t count) {
    OfsNode file;
    int rc = ofs_path(&ifs_ofs_active, path, 1, &file);
    return rc ? rc : ofs_write_checked(&ifs_ofs_active, &file, offset, bytes, count);
}
static int native_resize(const char *path, uint32_t size) {
    OfsNode file;
    int rc = ofs_path(&ifs_ofs_active, path, 1, &file);
    return rc ? rc : ofs_resize_checked(&ifs_ofs_active, &file, size);
}
static int native_sync(void) {
    OfsFs *fs = &ifs_ofs_active;
    if (!fs->mounted) return IFS_OFS_INVALID;
    if (fs->io.flags & IFS_OFS_ORDERED_WRITEBACK) return ofs_sync_pending(fs);
    if (fs->io.flags & IFS_OFS_READ_ONLY) return IFS_OFS_OK;
    return fs->io.flush(fs->io.context) == 0 ? IFS_OFS_OK : IFS_OFS_IO_ERROR;
}

static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant) {
    IfsOfsSpaceV3 space = {0};
    int r, closed;
    if (!variant) return IFS_FS_INVALID;
    *variant = 0u;
    if (!eligible(h)) return IFS_FS_INVALID;
    if (session.mounted || ifs_ofs_active.mounted) return IFS_FS_BUSY;
    IfsOfsIoV3 io=native(h,0);
    r=ofs_probe(&ifs_ofs_active,&io,NULL);
    if (r) return r;
    space.structure_bytes=sizeof(space);
    r=ofs_space(&ifs_ofs_active,&space);
    closed=native_unmount();
    if (r || closed) return r ? r : closed;
    *variant=space.variant;
    return IFS_FS_OK;
}
static int guarded_probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant)
{
    if (variant) *variant=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(probe(h,variant));
}

static int mount(const IfsFsHostRuntimeV2 *h, void **v, IfsFsVolumeInfoV2 *info) {
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
    /* Preserve the caller's size-versioned V2 prefix without reading
     * optional fields that an earlier host did not allocate. */
    size_t host_bytes=h->structure_bytes<sizeof(session.host) ?
        h->structure_bytes : sizeof(session.host);
    memset(&session.host,0,sizeof(session.host));
    memcpy(&session.host,h,host_bytes);
    session.host.structure_bytes=(uint32_t)host_bytes;
    IfsOfsIoV3 io=native(&session.host,write);
    r=ofs_probe(&ifs_ofs_active,&io,NULL);
    if(r) return r;
    space.structure_bytes=sizeof(space);
    r=ofs_space(&ifs_ofs_active,&space);
    if(r) { (void)native_unmount(); return r; }
    session.mounted=1;session.writable=write;session.allocation_verified=0;
    session.handle=++session.generation;
    info->variant=space.variant;
    info->block_bytes=space.block_bytes;
    /* Latin-1 disk characters may occupy two UTF-8 bytes. */
    info->max_name_bytes=space.variant==6u?212u:60u;
    info->total_bytes=space.total_bytes;info->free_bytes=space.free_bytes;
    *v=(void *)session.handle;
    return IFS_FS_OK;
}
static int guarded_mount(const IfsFsHostRuntimeV2 *h, void **v, IfsFsVolumeInfoV2 *info)
{
    if (v) *v=NULL;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(mount(h,v,info));
}

static int unmount(void *v) {
    if(!live(v))return IFS_FS_INVALID;
    /* native_unmount always releases the native engine, even when its
     * final durability barrier fails. Invalidate the Common V2 handle on
     * both paths, preserving the I/O failure for the caller. */
    int r=native_unmount();
    session.mounted=0;
    session.writable=0;
    session.handle=0;
    return r;
}
static int guarded_unmount(void *v)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(unmount(v));
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
    int r=native_stat(p,&e);
    return r?r:assign(&e,out);
}
static int guarded_stat_path(void *v, const char *p, IfsFsEntryV2 *out)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(stat_path(v,p,out));
}

static int nth(void *v,const char *p,uint32_t index,IfsFsEntryV2 *out){
    IfsOfsEntryV3 e={0};
    if(!live(v)||!p||!out)return IFS_FS_INVALID;
    e.structure_bytes=sizeof(e);
    int r=native_nth(p,index,&e);
    return r?r:assign(&e,out);
}
static int guarded_nth(void *v,const char *p,uint32_t index,IfsFsEntryV2 *out)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(nth(v,p,index,out));
}

static int read_file(void *v,const char *p,uint64_t at,void *b,uint32_t n){
    if(!live(v)||!p||(!b&&n))return IFS_FS_INVALID;
    if(at>UINT32_MAX||n>UINT32_MAX-at)return IFS_FS_UNSUPPORTED;
    return native_read(p,(uint32_t)at,b,n);
}
static int guarded_read_file(void *v,const char *p,uint64_t at,void *b,uint32_t n)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(read_file(v,p,at,b,n));
}

static int write_file(void *v,const char *p,uint64_t at,const void *b,uint32_t n){
    if(!live(v)||!p||(!b&&n))return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    if(at>UINT32_MAX||n>UINT32_MAX-at)return IFS_FS_UNSUPPORTED;
    return native_write(p,(uint32_t)at,b,n);
}
static int guarded_write_file(void *v,const char *p,uint64_t at,const void *b,uint32_t n)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    session.allocation_verified=0;
    return v2_leave_operation(write_file(v,p,at,b,n));
}

static int create_path(void *v,const char *p,uint32_t type){
    if(!live(v)||!p)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    if(type==IFS_FS_ENTRY_FILE)return ofs_create(&ifs_ofs_active,p,IFS_OFS_FILE,0,0);
    if(type==IFS_FS_ENTRY_DIRECTORY)return ofs_create(&ifs_ofs_active,p,IFS_OFS_DIRECTORY,0,0);
    return IFS_FS_UNSUPPORTED;
}
static int guarded_create_path(void *v,const char *p,uint32_t type)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    session.allocation_verified=0;
    return v2_leave_operation(create_path(v,p,type));
}

static int remove_path(void *v,const char *p) {
    if(!live(v)||!p)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    return ofs_unlink(&ifs_ofs_active,p);
}
static int guarded_remove_path(void *v,const char *p)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    session.allocation_verified=0;
    return v2_leave_operation(remove_path(v,p));
}

static int rename_path(void *v,const char *a,const char *b){
    if(!live(v)||!a||!b)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    return ofs_rename(&ifs_ofs_active,a,b);
}
static int guarded_rename_path(void *v,const char *a,const char *b)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    session.allocation_verified=0;
    return v2_leave_operation(rename_path(v,a,b));
}

static int resize_path(void *v,const char *p,uint64_t bytes){
    if(!live(v)||!p)return IFS_FS_INVALID;
    if(!can_write())return IFS_FS_ACCESS;
    if(bytes>UINT32_MAX)return IFS_FS_UNSUPPORTED;
    return native_resize(p,(uint32_t)bytes);
}
static int guarded_resize_path(void *v,const char *p,uint64_t bytes)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    session.allocation_verified=0;
    return v2_leave_operation(resize_path(v,p,bytes));
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
    return ofs_format(&ifs_ofs_active,&io,&options);
}
static int guarded_format(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                  const char *label,uint32_t flags)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(format(h,variant,label,flags));
}

static int check(void *v,uint32_t *errors){
    IfsOfsCheckV3 report={0};
    if(!live(v)||!errors)return IFS_FS_INVALID;
    report.structure_bytes=sizeof(report);
    int r=ofs_verify(&ifs_ofs_active,&report,OFS_VERIFY_FULL);
    *errors=report.errors;
    return r;
}
/* Common V2 is the sole exported format-neutral contract. Its operations
 * use the authoritative OFS V3 engine and the verified V2 host lease.
 * Relocation and recovery remain unadvertised; DEFRAG V2 exposes only native file mapping. */
static int volume_query(void *mounted,IfsFsVolumeInfoV2 *info) {
    IfsOfsSpaceV3 space={0};
    if(!live(mounted)||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    space.structure_bytes=sizeof(space);
    int r=ofs_space(&ifs_ofs_active,&space);
    if(r!=IFS_FS_OK)return r;
    info->variant=space.variant;
    info->block_bytes=space.block_bytes;
    /* Latin-1 disk characters may occupy two UTF-8 bytes. */
    info->max_name_bytes=space.variant==6u?212u:60u;
    info->total_bytes=space.total_bytes;
    info->free_bytes=space.free_bytes;
    return IFS_FS_OK;
}
static int guarded_volume_query(void *mounted,IfsFsVolumeInfoV2 *info)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_query(mounted,info));
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
static int guarded_volume_label(void *mounted, char *out, uint32_t capacity, uint32_t *bytes)
{
    if (bytes) *bytes=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_label(mounted,out,capacity,bytes));
}

static int volume_sync(void *mounted) {
    if(!live(mounted))return IFS_FS_INVALID;
    return native_sync()==0?
        IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int guarded_volume_sync(void *mounted)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_sync(mounted));
}

static int volume_check(void *mounted,uint32_t flags,uint64_t *errors) {
    uint32_t count=0;
    if(flags)return IFS_FS_UNSUPPORTED;
    if(!errors)return IFS_FS_INVALID;
    const int r=check(mounted,&count);
    if(r==IFS_FS_OK)*errors=count;
    return r;
}
static int guarded_volume_check(void *mounted,uint32_t flags,uint64_t *errors)
{
    if (errors) *errors=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_check(mounted,flags,errors));
}

/* Native OFS file extent mapping. A physical extent identifies the payload
 * following the 24-byte OFS data header, not an FFS-style raw data block.
 * Reuse the checked native chain cursor; metadata mutations already
 * invalidate these read cursors. A complete successful page is published
 * only after validating its table pointers, headers and block checksums.
 */
static int map_file(void *mounted,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!live(mounted)||!path||!out||!capacity||capacity>64u)
        return IFS_FS_INVALID;
    OfsFs *f=&ifs_ofs_active;
    OfsNode node;
    int rc=ofs_path(f,path,1,&node);
    if(rc)return rc;
    if(node.object_type!=IFS_OFS_FILE)return IFS_FS_UNSUPPORTED;
    if(node.protection&8u)return IFS_FS_ACCESS;
    if(f->block_bytes<=24u)return IFS_FS_CORRUPT;
    const uint32_t unit=f->block_bytes-24u;
    const uint32_t total=node.bytes?((node.bytes-1u)/unit+1u):0u;
    if(first>=total)return IFS_FS_END;
    const uint32_t count=(uint32_t)((total-first)<capacity?total-first:capacity);
    OfsFileCursor cursor={0};
    cursor.owner=node.object;cursor.size=node.bytes;
    uint32_t slot=f->read_cursor_count;
    for(uint32_t i=0u;i<f->read_cursor_count;++i) {
        if(f->read_cursors[i].owner==node.object &&
           f->read_cursors[i].size==node.bytes) {
            slot=i;
            if(f->read_cursors[i].sequence<=first)cursor=f->read_cursors[i];
            break;
        }
    }
    IfsFsExtentV2 page[64]={{0}};
    uint8_t *data=f->scratch[4];
    for(uint32_t i=0u;i<count;++i) {
        const uint32_t index=(uint32_t)first+i;
        uint32_t key=0u;
        rc=ofs_pointer(f,node.object,index,total,&cursor,&key);
        if(rc)return rc;
        if(cursor.sequence==index && cursor.sequence && cursor.next!=key)
            return IFS_FS_CORRUPT;
        rc=ofs_data(f,node.object,node.bytes,index,key,data);
        if(rc)return rc;
        int free_block=0;
        rc=ofs_bitmap_free(f,key,&free_block);
        if(rc)return rc;
        if(free_block)return IFS_FS_CORRUPT;
        const uint64_t logical=(uint64_t)index*unit;
        const uint64_t physical=(uint64_t)key*f->block_bytes+24u;
        const uint64_t length=node.bytes-logical<unit?node.bytes-logical:unit;
        if(physical>f->io.volume_bytes ||
           length>f->io.volume_bytes-physical)return IFS_FS_CORRUPT;
        page[i].logical_offset=logical;
        page[i].physical_offset=physical;
        page[i].length=length;
        cursor.sequence=index+1u;
        cursor.block=key;
        cursor.next=ofs_word(data,4u);
    }
    if(slot==f->read_cursor_count) {
        if(slot<OFS_APPEND_CACHE_SLOTS)++f->read_cursor_count;
        else slot=(f->read_cursor_next++)%OFS_APPEND_CACHE_SLOTS;
    }
    f->read_cursors[slot]=cursor;
    memcpy(out,page,count*sizeof(*page));
    *written=count;
    return IFS_FS_OK;
}
static int guarded_map_file(void *mounted,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if(written)*written=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(map_file(mounted,path,first,out,capacity,written));
}
/* Allocation is obtained from the native verified bitmap, never inferred
 * from a file map. The exclusive writable lease or immutable read snapshot
 * bounds the validity of the cached ownership check. All mutations revoke it.
 * Limit each page's scan even for a completely empty multi-gigabyte volume.
 */
static int allocation_map(void *mounted,uint64_t first,
                          IfsFsAllocationRangeV2 *out,uint32_t capacity,
                          uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!live(mounted)||!out||!capacity||capacity>64u)return IFS_FS_INVALID;
    OfsFs *f=&ifs_ofs_active;
    const uint64_t limit=session.host.volume_bytes;
    if(first>limit)return IFS_FS_INVALID;
    if(f->failed||f->mutation||!f->bitmap_valid)return IFS_FS_CORRUPT;
    if(!first||!session.allocation_verified) {
        IfsOfsCheckV3 report={0};
        int rc=ofs_verify(f,&report,OFS_VERIFY_ALLOCATION);
        if(rc)return rc;
        session.allocation_verified=1;
    }
    if(first==limit)return IFS_FS_END;
    IfsFsAllocationRangeV2 page[64]={{0}};
    uint32_t count=0u, scanned=0u;
    uint64_t at=first;
    while(at<limit && scanned<65536u) {
        if(session.host.cancelled && session.host.cancelled(session.host.context))
            return IFS_FS_BUSY;
        const uint64_t block=at/f->block_bytes;
        uint64_t end;
        uint32_t state;
        if(block<f->reserved) {
            state=IFS_FS_SPACE_RESERVED;
            end=(uint64_t)f->reserved*f->block_bytes;
        } else if(block>=f->blocks) {
            state=IFS_FS_SPACE_RESERVED;
            end=limit;
        } else {
            int is_free=0;
            int rc=ofs_bitmap_free(f,(uint32_t)block,&is_free);
            if(rc)return rc;
            state=is_free?IFS_FS_SPACE_FREE:IFS_FS_SPACE_ALLOCATED;
            end=(block+1u)*f->block_bytes;
        }
        if(end>limit)end=limit;
        if(count && page[count-1u].state==state) {
            page[count-1u].length+=end-at;
        } else {
            if(count==capacity)break;
            page[count++]=(IfsFsAllocationRangeV2){
                .physical_offset=at,.length=end-at,.state=state};
        }
        at=end;
        ++scanned;
    }
    memcpy(out,page,count*sizeof(*page));
    *written=count;
    return IFS_FS_OK;
}
static int guarded_allocation_map(void *mounted,uint64_t first,
                          IfsFsAllocationRangeV2 *out,uint32_t capacity,
                          uint32_t *written)
{
    if(written)*written=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(allocation_map(mounted,first,out,capacity,written));
}
static const IfsFsAllocationOpsV2 allocation_v2={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsAllocationOpsV2),
             IFS_FS_GROUP_ALLOCATION,0u,IFS_FS_ALLOCATION_MAP},
    .map=guarded_allocation_map
};
static const IfsFsDefragOpsV2 map_v2={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_MAP},
    .map=guarded_map_file
};
__attribute__((used,section(".ifsmeta")))
static const char identity_v2[]="IFS-MODULE:ofs:common2";
static const IfsFsCoreOpsV2 core_v2={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
       IFS_FS_GROUP_CORE,0u,
       IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
       IFS_FS_CORE_LIST|IFS_FS_CORE_READ|IFS_FS_CORE_WRITE|
       IFS_FS_CORE_CREATE|IFS_FS_CORE_REMOVE|IFS_FS_CORE_RENAME|
       IFS_FS_CORE_TRUNCATE},
    .probe=guarded_probe,.mount=guarded_mount,.unmount=guarded_unmount,
    .stat=guarded_stat_path,.directory_entry=guarded_nth,
    .read=guarded_read_file,.write=guarded_write_file,.create=guarded_create_path,
    .remove=guarded_remove_path,.rename=guarded_rename_path,.truncate=guarded_resize_path
};
static const IfsFsVolumeOpsV2 volume_v2={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
       IFS_FS_GROUP_VOLUME,0u,
       IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET|IFS_FS_VOLUME_SYNC|
       IFS_FS_VOLUME_CHECK|IFS_FS_VOLUME_FORMAT},
    .query=guarded_volume_query,.label=guarded_volume_label,.sync=guarded_volume_sync,
    .check=guarded_volume_check,.format=guarded_format
};
static const IfsFsGroupHeaderV2 *query_v2(
    uint32_t id,uint32_t low,uint32_t high){
    if(low>IFS_FS_GROUP_ABI_V2||high<IFS_FS_GROUP_ABI_V2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core_v2.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume_v2.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &map_v2.header;
    if(id==IFS_FS_GROUP_ALLOCATION)return &allocation_v2.header;
    return NULL;
}
static const IfsFsModuleApiV2 api_v2={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"ofs",
    IFS_FS_PROFILE_HOSTED|IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION),query_v2
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){
    return &api_v2;
}
