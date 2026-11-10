/* SPDX-License-Identifier: GPL-3.0-or-later
 * Infiltratr Common V2: universal, freestanding filesystem contracts.
 * V1 remains binary-immutable. This header contains NO disk format engine,
 * Linux kernel types, EFI imports, libc function calls or loader code.
 */
#ifndef INFILTRATR_FILESYSTEM_MODULE_V2_H
#define INFILTRATR_FILESYSTEM_MODULE_V2_H
#include "filesystem_module.h"
#ifdef __cplusplus
extern "C" {
#endif

#define IFS_FS_COMMON_ABI_V2 2u
#define IFS_FS_COMMON_EXPORT_V2 "infiltrator_filesystem_api_v2"
#define IFS_FS_GROUP_ABI_V1 1u
#define IFS_FS_HOST_ABI_V2 2u

/* Profile bits describe ABI/execution contracts, NOT evidence that a host
 * has verified an ELF image, authenticated a module or mounted a root FS. */
#define IFS_FS_PROFILE_HOSTED       (UINT64_C(1) << 0)
#define IFS_FS_PROFILE_FREESTANDING (UINT64_C(1) << 1)
#define IFS_FS_PROFILE_KERNEL       (UINT64_C(1) << 2)
#define IFS_FS_PROFILE_FIRMWARE     (UINT64_C(1) << 3)
#define IFS_FS_PROFILE_ALL (IFS_FS_PROFILE_HOSTED | IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_KERNEL | IFS_FS_PROFILE_FIRMWARE)

/* Group IDs never change meaning. A new group version extends the existing
 * prefix; a new category receives a new ID. Unknown groups fail closed. */
#define IFS_FS_GROUP_HANDLES    1u
#define IFS_FS_GROUP_METADATA   2u
#define IFS_FS_GROUP_VOLUME     3u
#define IFS_FS_GROUP_DEFRAG     4u
#define IFS_FS_GROUP_STREAMS    5u
#define IFS_FS_GROUP_SNAPSHOTS  6u
#define IFS_FS_GROUP_MAX       6u
#define IFS_FS_GROUP_BIT(id) (UINT64_C(1) << ((id) - 1u))
#define IFS_FS_GROUP_ALL ((UINT64_C(1) << IFS_FS_GROUP_MAX) - 1u)

/* Environment-specific hosts supply platform services directly. A kernel
 * adapter may back these with VFS/block/memory primitives; a UEFI adapter
 * with firmware BlockIo. Neither imports a userspace service process. */
#define IFS_FS_HOST_READ_ONLY   (UINT64_C(1) << 0)
#define IFS_FS_HOST_EXCLUSIVE   (UINT64_C(1) << 1)
#define IFS_FS_HOST_DURABLE     (UINT64_C(1) << 2)
#define IFS_FS_HOST_DISCARD     (UINT64_C(1) << 3)
#define IFS_FS_HOST_LOCKS       (UINT64_C(1) << 4)
#define IFS_FS_HOST_CLOCK       (UINT64_C(1) << 5)
#define IFS_FS_HOST_CANCEL      (UINT64_C(1) << 6)
#define IFS_FS_HOST_LOG         (UINT64_C(1) << 7)
#define IFS_FS_HOST_ALL ((UINT64_C(1) << 8) - 1u)

typedef struct {
    uint32_t abi_version, structure_bytes;
    uint64_t capabilities, volume_bytes;
    uint32_t logical_sector_bytes, physical_sector_bytes, maximum_transfer_bytes;
    uint32_t reserved; /* zero; partition-relative I/O and verified geometry */
    void *context;
    int (*read_at)(void *, uint64_t offset, void *, uint32_t bytes);
    int (*write_at)(void *, uint64_t offset, const void *, uint32_t bytes);
    int (*flush)(void *); /* durable barrier, not merely a cache write */
    int (*discard)(void *, uint64_t offset, uint64_t bytes);
    void *(*allocate)(void *, size_t bytes);
    void (*release)(void *, void *);
    void *(*create_lock)(void *);
    void (*destroy_lock)(void *, void *);
    void (*lock)(void *, void *);
    void (*unlock)(void *, void *);
    uint64_t (*monotonic_ns)(void *);
    uint64_t (*realtime_ns)(void *);
    int (*cancelled)(void *);
    void (*log)(void *, uint32_t severity, const char *message);
} IfsFsHostRuntimeV2;

/* A filesystem must not trust an asserted exclusivity flag without the
 * privileged host actually obtaining an unmounted, exclusive device lease.
 * Read-only hosts can have no write callback. Do not invoke untrusted code
 * merely to discover an ABI; validate and authenticate the binary first. */
static inline int ifs_fs_host_v2_valid(const IfsFsHostRuntimeV2 *h)
{
    uint32_t l, p;
    if (!h || h->abi_version != IFS_FS_HOST_ABI_V2 ||
        h->structure_bytes < sizeof(*h) || h->reserved ||
        (h->capabilities & ~IFS_FS_HOST_ALL) ||
        !h->read_at || !h->allocate || !h->release ||
        h->volume_bytes == 0u || h->maximum_transfer_bytes == 0u)
        return 0;
    l = h->logical_sector_bytes;
    p = h->physical_sector_bytes;
    if (l < 512u || l > 65536u || (l & (l - 1u)) ||
        p < l || p > 65536u || (p & (p - 1u)) || p % l ||
        h->maximum_transfer_bytes < l)
        return 0;
    if ((h->capabilities & IFS_FS_HOST_READ_ONLY) && h->write_at)
        return 0;
    if (!(h->capabilities & IFS_FS_HOST_READ_ONLY) &&
        (!(h->capabilities & IFS_FS_HOST_EXCLUSIVE) || !h->write_at ||
         !h->flush || !(h->capabilities & IFS_FS_HOST_DURABLE)))
        return 0;
    if (((h->capabilities & IFS_FS_HOST_DISCARD) != 0) !=
         (h->discard != NULL) ||
        ((h->capabilities & IFS_FS_HOST_CLOCK) != 0) !=
         (h->monotonic_ns != NULL && h->realtime_ns != NULL) ||
        ((h->capabilities & IFS_FS_HOST_CANCEL) != 0) !=
         (h->cancelled != NULL) ||
        ((h->capabilities & IFS_FS_HOST_LOG) != 0) !=
         (h->log != NULL))
        return 0;
    if ((h->capabilities & IFS_FS_HOST_LOCKS) ?
        (!h->create_lock || !h->destroy_lock || !h->lock || !h->unlock) :
        (h->create_lock || h->destroy_lock || h->lock || h->unlock))
        return 0;
    return 1;
}

/* Safe cross-version host adaptation for canonical engines that still use
 * V1 callbacks. The host descriptor is copied; it never proves a lease on
 * its own. Its caller must keep the host context alive through unmount.
 * A read-only host never exposes a write or flush mutation callback.
 * No allocations, device access or format-specific logic occur here. */
static inline int ifs_fs_host_v2_as_v1(
    const IfsFsHostRuntimeV2 *host, IfsFsHostIoV1 *out)
{
    if (!out || !ifs_fs_host_v2_valid(host)) return IFS_FS_INVALID;
    out->structure_bytes = sizeof(*out);
    out->flags = (host->capabilities & IFS_FS_HOST_READ_ONLY)
        ? IFS_FS_IO_READ_ONLY : IFS_FS_IO_EXCLUSIVE;
    out->context = host->context;
    out->volume_bytes = host->volume_bytes;
    out->read_at = host->read_at;
    out->write_at = (host->capabilities & IFS_FS_HOST_READ_ONLY)
        ? NULL : host->write_at;
    out->flush = (host->capabilities & IFS_FS_HOST_READ_ONLY)
        ? NULL : host->flush;
    out->allocate = host->allocate;
    out->release = host->release;
    return IFS_FS_OK;
}
static inline int ifs_fs_host_v2_as_format_v1(
    const IfsFsHostRuntimeV2 *host, IfsFsHostFormatIoV1 *out)
{
    int result;
    if (!out) return IFS_FS_INVALID;
    result = ifs_fs_host_v2_as_v1(host, &out->host);
    if (result != IFS_FS_OK) return result;
    out->host.structure_bytes = sizeof(*out);
    out->logical_sector_bytes = host->logical_sector_bytes;
    out->reserved = 0u;
    return IFS_FS_OK;
}

typedef struct {
    uint32_t abi_version, structure_bytes, group_id, reserved;
    uint64_t capabilities;
} IfsFsGroupHeaderV2;

/* Persistent open handles, cursors, fsync and handle-oriented VFS operations.
 * Handles are opaque and valid only for their owning volume and generation. */
#define IFS_FS_HANDLE_OPEN     (UINT64_C(1) << 0)
#define IFS_FS_HANDLE_READ     (UINT64_C(1) << 1)
#define IFS_FS_HANDLE_WRITE    (UINT64_C(1) << 2)
#define IFS_FS_HANDLE_RESIZE   (UINT64_C(1) << 3)
#define IFS_FS_HANDLE_SYNC     (UINT64_C(1) << 4)
#define IFS_FS_HANDLE_DIR      (UINT64_C(1) << 5)
#define IFS_FS_HANDLE_ALL      ((UINT64_C(1) << 6) - 1u)
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*open)(void *volume, const char *path, uint32_t flags, void **file);
    int (*close)(void *volume, void *file);
    int (*pread)(void *volume, void *file, uint64_t offset,
                 void *destination, uint32_t capacity, uint32_t *read_bytes);
    int (*pwrite)(void *volume, void *file, uint64_t offset,
                  const void *source, uint32_t length, uint32_t *written_bytes);
    int (*resize)(void *volume, void *file, uint64_t length);
    int (*sync)(void *volume, void *file, uint32_t flags);
    int (*open_dir)(void *volume, const char *path, void **cursor);
    int (*next_dir)(void *volume, void *cursor, IfsFsEntryV1 *entry);
    int (*close_dir)(void *volume, void *cursor);
} IfsFsHandlesV1;

#define IFS_FS_META_GET_TIMES    (UINT64_C(1) << 0)
#define IFS_FS_META_SET_TIMES    (UINT64_C(1) << 1)
#define IFS_FS_META_GET_MODE     (UINT64_C(1) << 2)
#define IFS_FS_META_SET_MODE     (UINT64_C(1) << 3)
#define IFS_FS_META_SYMLINK      (UINT64_C(1) << 4)
#define IFS_FS_META_READLINK     (UINT64_C(1) << 5)
#define IFS_FS_META_HARDLINK     (UINT64_C(1) << 6)
#define IFS_FS_META_GET_XATTR    (UINT64_C(1) << 7)
#define IFS_FS_META_SET_XATTR    (UINT64_C(1) << 8)
#define IFS_FS_META_GET_ACL      (UINT64_C(1) << 9)
#define IFS_FS_META_SET_ACL      (UINT64_C(1) << 10)
#define IFS_FS_META_ALL          ((UINT64_C(1) << 11) - 1u)
/* Host-to-provider times use signed Unix epoch nanoseconds, never native
 * OS timespec or EFI_TIME layouts; unsupported fields remain absent. */
typedef struct {
    uint32_t structure_bytes, mask;
    int64_t created_ns, modified_ns, accessed_ns, changed_ns;
    uint32_t mode, owner, group, attributes;
} IfsFsMetadataV1;
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*get)(void *volume, const char *path, IfsFsMetadataV1 *);
    int (*set)(void *volume, const char *path, const IfsFsMetadataV1 *);
    int (*symlink)(void *volume, const char *path, const char *target);
    int (*readlink)(void *volume, const char *path, char *, uint32_t, uint32_t *);
    int (*hardlink)(void *volume, const char *existing, const char *new_path);
    int (*get_xattr)(void *volume, const char *path, const char *key,
                     void *data, uint32_t capacity, uint32_t *bytes);
    int (*set_xattr)(void *volume, const char *path, const char *key,
                     const void *data, uint32_t bytes, uint32_t flags);
    int (*get_acl)(void *volume, const char *path,
                   void *data, uint32_t capacity, uint32_t *bytes);
    int (*set_acl)(void *volume, const char *path,
                   const void *data, uint32_t bytes);
} IfsFsMetadataOpsV1;

/* Destructive operations are NEVER automatic during discovery; safe format
 * requires an independently verified host lease and complete geometry. */
#define IFS_FS_VOLUME_QUERY    (UINT64_C(1) << 0)
#define IFS_FS_VOLUME_LABEL_GET (UINT64_C(1) << 1)
#define IFS_FS_VOLUME_LABEL_SET (UINT64_C(1) << 2)
#define IFS_FS_VOLUME_SYNC     (UINT64_C(1) << 3)
#define IFS_FS_VOLUME_RESIZE   (UINT64_C(1) << 4)
#define IFS_FS_VOLUME_CHECK    (UINT64_C(1) << 5)
#define IFS_FS_VOLUME_REPAIR   (UINT64_C(1) << 6)
#define IFS_FS_VOLUME_SCRUB    (UINT64_C(1) << 7)
#define IFS_FS_VOLUME_TRIM     (UINT64_C(1) << 8)
#define IFS_FS_VOLUME_FORMAT   (UINT64_C(1) << 9)
#define IFS_FS_VOLUME_ALL      ((UINT64_C(1) << 10) - 1u)
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*query)(void *volume, IfsFsVolumeInfoV1 *);
    int (*label)(void *volume, char *, uint32_t capacity, uint32_t *bytes);
    int (*set_label)(void *volume, const char *);
    int (*sync)(void *volume);
    int (*resize)(void *volume, uint64_t bytes, uint32_t flags);
    int (*check)(void *volume, uint32_t flags, uint64_t *errors);
    int (*repair)(void *volume, uint32_t flags, uint64_t *repairs);
    int (*scrub)(void *volume, uint32_t flags, uint64_t *errors);
    int (*trim)(void *volume, uint64_t offset, uint64_t length);
    int (*format)(const IfsFsHostRuntimeV2 *, uint32_t variant,
                  const char *label, uint32_t flags);
} IfsFsVolumeOpsV1;

/* The filesystem owns extent mapping/relocation and durable recovery.
 * Defragmenter supplies intent and policy; it must NOT move raw clusters. */
#define IFS_FS_DEFRAG_MAP      (UINT64_C(1) << 0)
#define IFS_FS_DEFRAG_PLAN     (UINT64_C(1) << 1)
#define IFS_FS_DEFRAG_EXECUTE  (UINT64_C(1) << 2)
#define IFS_FS_DEFRAG_VERIFY   (UINT64_C(1) << 3)
#define IFS_FS_DEFRAG_RECOVER  (UINT64_C(1) << 4)
#define IFS_FS_DEFRAG_ALL      ((UINT64_C(1) << 5) - 1u)
typedef struct {
    uint64_t logical_offset, physical_offset, length;
    uint32_t flags, reserved;
} IfsFsExtentV1;
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*map)(void *volume, const char *path, uint64_t first_extent,
               IfsFsExtentV1 *, uint32_t capacity, uint32_t *written);
    int (*plan)(void *volume, const char *path, uint32_t policy,
                void **plan);
    int (*execute)(void *volume, void *plan, uint64_t *moved_bytes);
    int (*verify)(void *volume, void *plan);
    int (*recover)(void *volume, uint32_t flags, uint64_t *repaired);
    void (*release_plan)(void *volume, void *plan);
} IfsFsDefragOpsV1;

/* Filesystems with named streams can export them without consumers parsing
 * NTFS ADS, macOS resource forks or format-specific stream metadata. */
#define IFS_FS_STREAM_LIST     (UINT64_C(1) << 0)
#define IFS_FS_STREAM_READ     (UINT64_C(1) << 1)
#define IFS_FS_STREAM_WRITE    (UINT64_C(1) << 2)
#define IFS_FS_STREAM_REMOVE   (UINT64_C(1) << 3)
#define IFS_FS_STREAM_ALL      ((UINT64_C(1) << 4) - 1u)
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*list)(void *volume, const char *path, uint32_t index,
                char *name, uint32_t capacity, uint32_t *bytes);
    int (*read)(void *volume, const char *path, const char *stream,
                uint64_t offset, void *, uint32_t length, uint32_t *read_bytes);
    int (*write)(void *volume, const char *path, const char *stream,
                 uint64_t offset, const void *, uint32_t length, uint32_t *written);
    int (*remove)(void *volume, const char *path, const char *stream);
} IfsFsStreamsOpsV1;

#define IFS_FS_SNAPSHOT_LIST   (UINT64_C(1) << 0)
#define IFS_FS_SNAPSHOT_CREATE (UINT64_C(1) << 1)
#define IFS_FS_SNAPSHOT_DELETE (UINT64_C(1) << 2)
#define IFS_FS_SNAPSHOT_REVERT (UINT64_C(1) << 3)
#define IFS_FS_SNAPSHOT_ALL    ((UINT64_C(1) << 4) - 1u)
typedef struct {
    uint64_t id, created_ns;
    uint32_t flags, reserved;
} IfsFsSnapshotV1;
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*list)(void *volume, uint32_t index, IfsFsSnapshotV1 *);
    int (*create)(void *volume, const char *label, uint64_t *id);
    int (*remove)(void *volume, uint64_t id);
    int (*revert)(void *volume, uint64_t id, uint32_t flags);
} IfsFsSnapshotOpsV1;

typedef const IfsFsGroupHeaderV2 *(*IfsFsQueryGroupV2)(
    uint32_t group_id, uint32_t minimum_revision, uint32_t maximum_revision);

/* Always export a truthful V1 base table. Extension groups are separately
 * negotiated and describe only real implemented operations. A valid V2
 * descriptor DOES NOT make a binary executable in arbitrary kernel/UEFI
 * context: that requires separately verified runtime profile and loader. */
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t runtime_profiles;
    uint64_t group_mask;
    const IfsFsModuleApiV1 *base_v1;
    IfsFsQueryGroupV2 query_group;
} IfsFsModuleApiV2;
typedef const IfsFsModuleApiV2 *(*IfsFsModuleApiEntryV2)(void);

static inline int ifs_fs_group_v2_header_valid(
    const IfsFsGroupHeaderV2 *g, uint32_t id, uint32_t minimum_bytes,
    uint64_t known_capabilities)
{
    return g && g->abi_version == IFS_FS_GROUP_ABI_V1 &&
        g->structure_bytes >= minimum_bytes && g->group_id == id &&
        g->reserved == 0u && g->capabilities != 0u &&
        !(g->capabilities & ~known_capabilities);
}

/* Group inspection invokes already trusted provider code. Never call this
 * on a freshly downloaded or unchecked module simply to read its identity. */
static inline const IfsFsGroupHeaderV2 *ifs_fs_group_v2(
    const IfsFsModuleApiV2 *module, uint32_t id)
{
    const IfsFsGroupHeaderV2 *g;
    uint32_t bytes = 0u;
    uint64_t capabilities = 0u;
    if (!module || id == 0u || id > IFS_FS_GROUP_MAX ||
        !(module->group_mask & IFS_FS_GROUP_BIT(id)) ||
        !module->query_group) return NULL;
    switch (id) {
    case IFS_FS_GROUP_HANDLES: bytes=sizeof(IfsFsHandlesV1);
        capabilities=IFS_FS_HANDLE_ALL; break;
    case IFS_FS_GROUP_METADATA: bytes=sizeof(IfsFsMetadataOpsV1);
        capabilities=IFS_FS_META_ALL; break;
    case IFS_FS_GROUP_VOLUME: bytes=sizeof(IfsFsVolumeOpsV1);
        capabilities=IFS_FS_VOLUME_ALL; break;
    case IFS_FS_GROUP_DEFRAG: bytes=sizeof(IfsFsDefragOpsV1);
        capabilities=IFS_FS_DEFRAG_ALL; break;
    case IFS_FS_GROUP_STREAMS: bytes=sizeof(IfsFsStreamsOpsV1);
        capabilities=IFS_FS_STREAM_ALL; break;
    case IFS_FS_GROUP_SNAPSHOTS: bytes=sizeof(IfsFsSnapshotOpsV1);
        capabilities=IFS_FS_SNAPSHOT_ALL; break;
    default: return NULL;
    }
    g = module->query_group(id, IFS_FS_GROUP_ABI_V1, IFS_FS_GROUP_ABI_V1);
    return ifs_fs_group_v2_header_valid(g,id,bytes,capabilities) ? g : NULL;
}

/* Descriptor and pointer-mask validation are separate from qualification of
 * the actual callback behaviour. Capabilities MUST agree with function
 * pointers; claiming an unsupported operation is a hard rejection. */
static inline int ifs_fs_group_v2_callbacks_valid(
    const IfsFsGroupHeaderV2 *g)
{
#define IFS_MATCH(mask, op) (((g->capabilities & (mask)) != 0u) == ((op) != 0))
    if (!g) return 0;
    switch (g->group_id) {
    case IFS_FS_GROUP_HANDLES: {
        const IfsFsHandlesV1 *p=(const IfsFsHandlesV1 *)g;
        return IFS_MATCH(IFS_FS_HANDLE_OPEN,p->open && p->close) &&
            IFS_MATCH(IFS_FS_HANDLE_READ,p->pread) &&
            IFS_MATCH(IFS_FS_HANDLE_WRITE,p->pwrite) &&
            IFS_MATCH(IFS_FS_HANDLE_RESIZE,p->resize) &&
            IFS_MATCH(IFS_FS_HANDLE_SYNC,p->sync) &&
            IFS_MATCH(IFS_FS_HANDLE_DIR,p->open_dir && p->next_dir && p->close_dir) &&
            (!(g->capabilities & (IFS_FS_HANDLE_READ|IFS_FS_HANDLE_WRITE|
              IFS_FS_HANDLE_RESIZE|IFS_FS_HANDLE_SYNC)) ||
              (g->capabilities & IFS_FS_HANDLE_OPEN));
    }
    case IFS_FS_GROUP_METADATA: {
        const IfsFsMetadataOpsV1 *p=(const IfsFsMetadataOpsV1 *)g;
        return (((g->capabilities & (IFS_FS_META_GET_TIMES |
               IFS_FS_META_GET_MODE)) != 0u) == (p->get != NULL)) &&
            (((g->capabilities & (IFS_FS_META_SET_TIMES |
               IFS_FS_META_SET_MODE)) != 0u) == (p->set != NULL)) &&
            IFS_MATCH(IFS_FS_META_SYMLINK,p->symlink) &&
            IFS_MATCH(IFS_FS_META_READLINK,p->readlink) &&
            IFS_MATCH(IFS_FS_META_HARDLINK,p->hardlink) &&
            IFS_MATCH(IFS_FS_META_GET_XATTR,p->get_xattr) &&
            IFS_MATCH(IFS_FS_META_SET_XATTR,p->set_xattr) &&
            IFS_MATCH(IFS_FS_META_GET_ACL,p->get_acl) &&
            IFS_MATCH(IFS_FS_META_SET_ACL,p->set_acl);
    }
    case IFS_FS_GROUP_VOLUME: {
        const IfsFsVolumeOpsV1 *p=(const IfsFsVolumeOpsV1 *)g;
        return IFS_MATCH(IFS_FS_VOLUME_QUERY,p->query) &&
            IFS_MATCH(IFS_FS_VOLUME_LABEL_GET,p->label) &&
            IFS_MATCH(IFS_FS_VOLUME_LABEL_SET,p->set_label) &&
            IFS_MATCH(IFS_FS_VOLUME_SYNC,p->sync) &&
            IFS_MATCH(IFS_FS_VOLUME_RESIZE,p->resize) &&
            IFS_MATCH(IFS_FS_VOLUME_CHECK,p->check) &&
            IFS_MATCH(IFS_FS_VOLUME_REPAIR,p->repair) &&
            IFS_MATCH(IFS_FS_VOLUME_SCRUB,p->scrub) &&
            IFS_MATCH(IFS_FS_VOLUME_TRIM,p->trim) &&
            IFS_MATCH(IFS_FS_VOLUME_FORMAT,p->format);
    }
    case IFS_FS_GROUP_DEFRAG: {
        const IfsFsDefragOpsV1 *p=(const IfsFsDefragOpsV1 *)g;
        return IFS_MATCH(IFS_FS_DEFRAG_MAP,p->map) &&
            IFS_MATCH(IFS_FS_DEFRAG_PLAN,p->plan && p->release_plan) &&
            IFS_MATCH(IFS_FS_DEFRAG_EXECUTE,p->execute) &&
            IFS_MATCH(IFS_FS_DEFRAG_VERIFY,p->verify) &&
            IFS_MATCH(IFS_FS_DEFRAG_RECOVER,p->recover) &&
            (!(g->capabilities & (IFS_FS_DEFRAG_EXECUTE|IFS_FS_DEFRAG_VERIFY)) ||
              (g->capabilities & IFS_FS_DEFRAG_PLAN));
    }
    case IFS_FS_GROUP_STREAMS: {
        const IfsFsStreamsOpsV1 *p=(const IfsFsStreamsOpsV1 *)g;
        return IFS_MATCH(IFS_FS_STREAM_LIST,p->list) &&
            IFS_MATCH(IFS_FS_STREAM_READ,p->read) &&
            IFS_MATCH(IFS_FS_STREAM_WRITE,p->write) &&
            IFS_MATCH(IFS_FS_STREAM_REMOVE,p->remove);
    }
    case IFS_FS_GROUP_SNAPSHOTS: {
        const IfsFsSnapshotOpsV1 *p=(const IfsFsSnapshotOpsV1 *)g;
        return IFS_MATCH(IFS_FS_SNAPSHOT_LIST,p->list) &&
            IFS_MATCH(IFS_FS_SNAPSHOT_CREATE,p->create) &&
            IFS_MATCH(IFS_FS_SNAPSHOT_DELETE,p->remove) &&
            IFS_MATCH(IFS_FS_SNAPSHOT_REVERT,p->revert);
    }
    default: return 0;
    }
#undef IFS_MATCH
}
static inline int ifs_fs_api_v2_valid(
    const IfsFsModuleApiV2 *a, const char *expected_id)
{
    uint32_t group;
    if (!a || a->abi_version != IFS_FS_COMMON_ABI_V2 ||
        a->structure_bytes < sizeof(*a) ||
        !ifs_fs_format_id_equal(a->format_id, expected_id) ||
        !a->runtime_profiles ||
        (a->runtime_profiles & ~IFS_FS_PROFILE_ALL) ||
        (a->group_mask & ~IFS_FS_GROUP_ALL) ||
        !ifs_fs_common_api_valid(a->base_v1, expected_id) ||
        ((a->group_mask != 0u) != (a->query_group != NULL)))
        return 0;
    for (group=1u;group<=IFS_FS_GROUP_MAX;++group)
        if ((a->group_mask & IFS_FS_GROUP_BIT(group)) &&
            !ifs_fs_group_v2_callbacks_valid(ifs_fs_group_v2(a,group)))
            return 0;
    return 1;
}
#ifdef __cplusplus
}
#endif
#endif
