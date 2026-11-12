/* SPDX-License-Identifier: GPL-3.0-or-later
 * Infiltratr Common V2: independent, freestanding filesystem contract.
 * No previous-ABI descriptor, compatibility fallback, or OS-specific import.
 */
#ifndef INFILTRATR_FILESYSTEM_MODULE_V2_H
#define INFILTRATR_FILESYSTEM_MODULE_V2_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define IFS_FS_MAX_FORMAT_ID 63u
#define IFS_FS_ENTRY_FILE 1u
#define IFS_FS_ENTRY_DIRECTORY 2u
#define IFS_FS_ENTRY_LINK 3u
#define IFS_FS_OK 0
#define IFS_FS_END 1
#define IFS_FS_NOT_FOUND 2
#define IFS_FS_INVALID (-1)
#define IFS_FS_IO_ERROR (-2)
#define IFS_FS_CORRUPT (-3)
#define IFS_FS_NO_SPACE (-4)
#define IFS_FS_EXISTS (-5)
#define IFS_FS_ACCESS (-6)
#define IFS_FS_NO_MEMORY (-7)
#define IFS_FS_UNSUPPORTED (-8)
#define IFS_FS_BUSY (-9)
static inline int ifs_fs_format_id_valid(const char *name)
{
    uint32_t i;
    if (!name) return 0;
    for (i = 0; i <= IFS_FS_MAX_FORMAT_ID; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!c) return i != 0;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 0;
}
static inline int ifs_fs_format_id_equal(const char *a, const char *b)
{
    uint32_t i;
    if (!ifs_fs_format_id_valid(a) || !ifs_fs_format_id_valid(b)) return 0;
    for (i = 0; i <= IFS_FS_MAX_FORMAT_ID; ++i) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}
#define IFS_FS_COMMON_ABI_V2 2u
#define IFS_FS_COMMON_EXPORT_V2 "infiltrator_filesystem_api_v2"
#define IFS_FS_GROUP_ABI_V2 2u
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
#define IFS_FS_GROUP_CORE       7u
#define IFS_FS_GROUP_MAX        7u
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

/* Portable byte-range reads over sector-constrained host I/O.
 * Callbacks receive ONLY complete, sector-aligned transfers, never
 * more than maximum_transfer_bytes. Misaligned/short requests use one
 * host-owned scratch sector and cannot cross the authenticated volume.
 * A read may have partially filled the destination before an I/O error.
 * No OS, libc, legacy ABI or filesystem-specific service is required. */
static inline int ifs_fs_host_read_bytes_v2(
    const IfsFsHostRuntimeV2 *host, uint64_t offset,
    void *destination, uint32_t bytes)
{
    uint8_t *out = (uint8_t *)destination;
    uint8_t *scratch = NULL;
    const uint32_t sector = host ? host->logical_sector_bytes : 0u;
    uint32_t transfer, chunk, skip, i;
    int result = IFS_FS_OK;
    if (!ifs_fs_host_v2_valid(host) || (!destination && bytes))
        return IFS_FS_INVALID;
    if (offset > host->volume_bytes ||
        bytes > host->volume_bytes - offset)
        return IFS_FS_INVALID;
    if (!bytes) return IFS_FS_OK;
    transfer = (host->maximum_transfer_bytes / sector) * sector;
    while (bytes != 0u) {
        skip = (uint32_t)(offset % sector);
        if (!skip && bytes >= sector) {
            chunk = bytes - bytes % sector;
            if (chunk > transfer) chunk = transfer;
            if (host->read_at(host->context, offset, out, chunk) != 0) {
                result = IFS_FS_IO_ERROR;
                break;
            }
        } else {
            uint64_t base = offset - skip;
            if (base > host->volume_bytes ||
                sector > host->volume_bytes - base) {
                result = IFS_FS_INVALID;
                break;
            }
            if (!scratch) {
                scratch = (uint8_t *)host->allocate(host->context, sector);
                if (!scratch) {
                    result = IFS_FS_NO_MEMORY;
                    break;
                }
            }
            if (host->read_at(host->context, base, scratch, sector) != 0) {
                result = IFS_FS_IO_ERROR;
                break;
            }
            chunk = sector - skip;
            if (chunk > bytes) chunk = bytes;
            for (i = 0u; i < chunk; ++i)
                out[i] = scratch[skip + i];
        }
        out += chunk;
        offset += chunk;
        bytes -= chunk;
    }
    if (scratch) host->release(host->context, scratch);
    return result;
}

typedef struct {
    uint32_t abi_version, structure_bytes, group_id, reserved;
    uint64_t capabilities;
} IfsFsGroupHeaderV2;

/* Core callbacks operate directly over the Common V2 host.
 * The host authenticates the module before any callback invocation.
 * No provider may declare a capability without its actual implementation. */
typedef struct {
    uint32_t structure_bytes, type;
    uint64_t size_bytes;
    uint32_t attributes, name_bytes;
    char *name;
    uint32_t name_capacity;
} IfsFsEntryV2;

typedef struct {
    uint32_t structure_bytes, variant, max_name_bytes, block_bytes;
    uint64_t total_bytes, free_bytes;
} IfsFsVolumeInfoV2;

#define IFS_FS_CORE_PROBE       (UINT64_C(1) << 0)
#define IFS_FS_CORE_MOUNT       (UINT64_C(1) << 1)
#define IFS_FS_CORE_STAT        (UINT64_C(1) << 2)
#define IFS_FS_CORE_LIST        (UINT64_C(1) << 3)
#define IFS_FS_CORE_READ        (UINT64_C(1) << 4)
#define IFS_FS_CORE_WRITE       (UINT64_C(1) << 5)
#define IFS_FS_CORE_CREATE      (UINT64_C(1) << 6)
#define IFS_FS_CORE_REMOVE      (UINT64_C(1) << 7)
#define IFS_FS_CORE_RENAME      (UINT64_C(1) << 8)
#define IFS_FS_CORE_TRUNCATE    (UINT64_C(1) << 9)
#define IFS_FS_CORE_ALL         ((UINT64_C(1) << 10) - 1u)
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*probe)(const IfsFsHostRuntimeV2 *host, uint32_t *variant);
    int (*mount)(const IfsFsHostRuntimeV2 *host, void **volume,
                 IfsFsVolumeInfoV2 *info);
    int (*unmount)(void *volume);
    int (*stat)(void *volume, const char *path, IfsFsEntryV2 *entry);
    int (*directory_entry)(void *volume, const char *path, uint32_t index,
                           IfsFsEntryV2 *entry);
    int (*read)(void *volume, const char *path, uint64_t offset,
                void *destination, uint32_t length);
    int (*write)(void *volume, const char *path, uint64_t offset,
                 const void *source, uint32_t length);
    int (*create)(void *volume, const char *path, uint32_t type);
    int (*remove)(void *volume, const char *path);
    int (*rename)(void *volume, const char *old_path, const char *new_path);
    int (*truncate)(void *volume, const char *path, uint64_t bytes);
} IfsFsCoreOpsV2;

/* Persistent open handles, cursors, fsync and handle-oriented VFS operations.
 * Handles are opaque and valid only for their owning volume and generation. */
#define IFS_FS_HANDLE_OPEN     (UINT64_C(1) << 0)
#define IFS_FS_HANDLE_READ     (UINT64_C(1) << 1)
#define IFS_FS_HANDLE_WRITE    (UINT64_C(1) << 2)
#define IFS_FS_HANDLE_RESIZE   (UINT64_C(1) << 3)
#define IFS_FS_HANDLE_SYNC     (UINT64_C(1) << 4)
#define IFS_FS_HANDLE_DIR      (UINT64_C(1) << 5)
#define IFS_FS_HANDLE_ALL      ((UINT64_C(1) << 6) - 1u)

/* Portable arguments to IfsFsHandlesV2.open(), never native O_* flags.
 * At least READ or WRITE is required. CREATE and TRUNCATE require WRITE;
 * EXCLUSIVE is meaningful only together with CREATE.
 *
 * CREATE: create a regular file if absent; do not truncate if present.
 * EXCLUSIVE|CREATE: return IFS_FS_EXISTS when the path already exists.
 * TRUNCATE: set an existing regular file's length to zero.
 * NOFOLLOW: reject symbolic links in path resolution rather than following
 * them; providers unable to guarantee that must return IFS_FS_UNSUPPORTED.
 * Providers must reject an unknown bit with IFS_FS_INVALID.
 */
#define IFS_FS_OPEN_READ       (UINT32_C(1) << 0)
#define IFS_FS_OPEN_WRITE      (UINT32_C(1) << 1)
#define IFS_FS_OPEN_CREATE     (UINT32_C(1) << 2)
#define IFS_FS_OPEN_EXCLUSIVE  (UINT32_C(1) << 3)
#define IFS_FS_OPEN_TRUNCATE   (UINT32_C(1) << 4)
#define IFS_FS_OPEN_NOFOLLOW   (UINT32_C(1) << 5)
#define IFS_FS_OPEN_ALL        ((UINT32_C(1) << 6) - 1u)
static inline int ifs_fs_open_flags_valid(uint32_t flags)
{
    return !(flags & ~IFS_FS_OPEN_ALL) &&
        (flags & (IFS_FS_OPEN_READ | IFS_FS_OPEN_WRITE)) &&
        (!(flags & (IFS_FS_OPEN_CREATE | IFS_FS_OPEN_TRUNCATE)) ||
         (flags & IFS_FS_OPEN_WRITE)) &&
        (!(flags & IFS_FS_OPEN_EXCLUSIVE) ||
         (flags & IFS_FS_OPEN_CREATE));
}

/* IfsFsHandlesV2.sync() accepts 0 for full durable data+metadata sync;
 * DATA_ONLY may omit unrelated metadata, but not metadata required for data
 * retrieval. Unknown sync flag bits must be rejected with IFS_FS_INVALID. */
#define IFS_FS_SYNC_DATA_ONLY  (UINT32_C(1) << 0)
#define IFS_FS_SYNC_ALL        IFS_FS_SYNC_DATA_ONLY

/* pread/pwrite return IFS_FS_OK with the number of bytes transferred in
 * their output parameter (never more than requested). A zero-byte successful
 * pread means EOF; directory next returns IFS_FS_END at exhaustion.
 * File/cursor handles are bound to their volume and mount generation. */
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
    int (*next_dir)(void *volume, void *cursor, IfsFsEntryV2 *entry);
    int (*close_dir)(void *volume, void *cursor);
} IfsFsHandlesV2;

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
} IfsFsMetadataV2;
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*get)(void *volume, const char *path, IfsFsMetadataV2 *);
    int (*set)(void *volume, const char *path, const IfsFsMetadataV2 *);
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
} IfsFsMetadataOpsV2;

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
    int (*query)(void *volume, IfsFsVolumeInfoV2 *);
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
} IfsFsVolumeOpsV2;

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
} IfsFsExtentV2;
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*map)(void *volume, const char *path, uint64_t first_extent,
               IfsFsExtentV2 *, uint32_t capacity, uint32_t *written);
    int (*plan)(void *volume, const char *path, uint32_t policy,
                void **plan);
    int (*execute)(void *volume, void *plan, uint64_t *moved_bytes);
    int (*verify)(void *volume, void *plan);
    int (*recover)(void *volume, uint32_t flags, uint64_t *repaired);
    void (*release_plan)(void *volume, void *plan);
} IfsFsDefragOpsV2;

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
} IfsFsStreamsOpsV2;

#define IFS_FS_SNAPSHOT_LIST   (UINT64_C(1) << 0)
#define IFS_FS_SNAPSHOT_CREATE (UINT64_C(1) << 1)
#define IFS_FS_SNAPSHOT_DELETE (UINT64_C(1) << 2)
#define IFS_FS_SNAPSHOT_REVERT (UINT64_C(1) << 3)
#define IFS_FS_SNAPSHOT_ALL    ((UINT64_C(1) << 4) - 1u)
typedef struct {
    uint64_t id, created_ns;
    uint32_t flags, reserved;
} IfsFsSnapshotV2;
typedef struct {
    IfsFsGroupHeaderV2 header;
    int (*list)(void *volume, uint32_t index, IfsFsSnapshotV2 *);
    int (*create)(void *volume, const char *label, uint64_t *id);
    int (*remove)(void *volume, uint64_t id);
    int (*revert)(void *volume, uint64_t id, uint32_t flags);
} IfsFsSnapshotOpsV2;

typedef const IfsFsGroupHeaderV2 *(*IfsFsQueryGroupV2)(
    uint32_t group_id, uint32_t minimum_revision, uint32_t maximum_revision);

/* Direct Common V2 descriptor. No previous version's table or shims.
 * Execution profiles are declarations requiring separate qualification. */
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t runtime_profiles;
    uint64_t group_mask;
    IfsFsQueryGroupV2 query_group;
} IfsFsModuleApiV2;
typedef const IfsFsModuleApiV2 *(*IfsFsModuleApiEntryV2)(void);

static inline int ifs_fs_group_v2_header_valid(
    const IfsFsGroupHeaderV2 *g, uint32_t id, uint32_t minimum_bytes,
    uint64_t known_capabilities)
{
    return g && g->abi_version == IFS_FS_GROUP_ABI_V2 &&
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
    case IFS_FS_GROUP_HANDLES: bytes=sizeof(IfsFsHandlesV2);
        capabilities=IFS_FS_HANDLE_ALL; break;
    case IFS_FS_GROUP_METADATA: bytes=sizeof(IfsFsMetadataOpsV2);
        capabilities=IFS_FS_META_ALL; break;
    case IFS_FS_GROUP_VOLUME: bytes=sizeof(IfsFsVolumeOpsV2);
        capabilities=IFS_FS_VOLUME_ALL; break;
    case IFS_FS_GROUP_DEFRAG: bytes=sizeof(IfsFsDefragOpsV2);
        capabilities=IFS_FS_DEFRAG_ALL; break;
    case IFS_FS_GROUP_STREAMS: bytes=sizeof(IfsFsStreamsOpsV2);
        capabilities=IFS_FS_STREAM_ALL; break;
    case IFS_FS_GROUP_SNAPSHOTS: bytes=sizeof(IfsFsSnapshotOpsV2);
        capabilities=IFS_FS_SNAPSHOT_ALL; break;
    case IFS_FS_GROUP_CORE: bytes=sizeof(IfsFsCoreOpsV2);
        capabilities=IFS_FS_CORE_ALL; break;
    default: return NULL;
    }
    g = module->query_group(id, IFS_FS_GROUP_ABI_V2, IFS_FS_GROUP_ABI_V2);
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
    case IFS_FS_GROUP_CORE: {
        const IfsFsCoreOpsV2 *p=(const IfsFsCoreOpsV2 *)g;
        return IFS_MATCH(IFS_FS_CORE_PROBE,p->probe) &&
            IFS_MATCH(IFS_FS_CORE_MOUNT,p->mount) &&
            IFS_MATCH(IFS_FS_CORE_MOUNT,p->unmount) &&
            IFS_MATCH(IFS_FS_CORE_STAT,p->stat) &&
            IFS_MATCH(IFS_FS_CORE_LIST,p->directory_entry) &&
            IFS_MATCH(IFS_FS_CORE_READ,p->read) &&
            IFS_MATCH(IFS_FS_CORE_WRITE,p->write) &&
            IFS_MATCH(IFS_FS_CORE_CREATE,p->create) &&
            IFS_MATCH(IFS_FS_CORE_REMOVE,p->remove) &&
            IFS_MATCH(IFS_FS_CORE_RENAME,p->rename) &&
            IFS_MATCH(IFS_FS_CORE_TRUNCATE,p->truncate) &&
            (!(g->capabilities & (IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|
                IFS_FS_CORE_READ|IFS_FS_CORE_WRITE|IFS_FS_CORE_CREATE|
                IFS_FS_CORE_REMOVE|IFS_FS_CORE_RENAME|
                IFS_FS_CORE_TRUNCATE)) ||
                ((g->capabilities & (IFS_FS_CORE_MOUNT|
                                     IFS_FS_CORE_STAT)) ==
                    (IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT)));
    }
    case IFS_FS_GROUP_HANDLES: {
        const IfsFsHandlesV2 *p=(const IfsFsHandlesV2 *)g;
        return IFS_MATCH(IFS_FS_HANDLE_OPEN,p->open) &&
            IFS_MATCH(IFS_FS_HANDLE_OPEN,p->close) &&
            IFS_MATCH(IFS_FS_HANDLE_READ,p->pread) &&
            IFS_MATCH(IFS_FS_HANDLE_WRITE,p->pwrite) &&
            IFS_MATCH(IFS_FS_HANDLE_RESIZE,p->resize) &&
            IFS_MATCH(IFS_FS_HANDLE_SYNC,p->sync) &&
            IFS_MATCH(IFS_FS_HANDLE_DIR,p->open_dir) &&
            IFS_MATCH(IFS_FS_HANDLE_DIR,p->next_dir) &&
            IFS_MATCH(IFS_FS_HANDLE_DIR,p->close_dir) &&
            (!(g->capabilities & (IFS_FS_HANDLE_READ|IFS_FS_HANDLE_WRITE|
              IFS_FS_HANDLE_RESIZE|IFS_FS_HANDLE_SYNC)) ||
              (g->capabilities & IFS_FS_HANDLE_OPEN));
    }
    case IFS_FS_GROUP_METADATA: {
        const IfsFsMetadataOpsV2 *p=(const IfsFsMetadataOpsV2 *)g;
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
        const IfsFsVolumeOpsV2 *p=(const IfsFsVolumeOpsV2 *)g;
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
        const IfsFsDefragOpsV2 *p=(const IfsFsDefragOpsV2 *)g;
        return IFS_MATCH(IFS_FS_DEFRAG_MAP,p->map) &&
            IFS_MATCH(IFS_FS_DEFRAG_PLAN,p->plan) &&
            IFS_MATCH(IFS_FS_DEFRAG_PLAN,p->release_plan) &&
            IFS_MATCH(IFS_FS_DEFRAG_EXECUTE,p->execute) &&
            IFS_MATCH(IFS_FS_DEFRAG_VERIFY,p->verify) &&
            IFS_MATCH(IFS_FS_DEFRAG_RECOVER,p->recover) &&
            (!(g->capabilities & (IFS_FS_DEFRAG_EXECUTE|IFS_FS_DEFRAG_VERIFY)) ||
              (g->capabilities & IFS_FS_DEFRAG_PLAN));
    }
    case IFS_FS_GROUP_STREAMS: {
        const IfsFsStreamsOpsV2 *p=(const IfsFsStreamsOpsV2 *)g;
        return IFS_MATCH(IFS_FS_STREAM_LIST,p->list) &&
            IFS_MATCH(IFS_FS_STREAM_READ,p->read) &&
            IFS_MATCH(IFS_FS_STREAM_WRITE,p->write) &&
            IFS_MATCH(IFS_FS_STREAM_REMOVE,p->remove);
    }
    case IFS_FS_GROUP_SNAPSHOTS: {
        const IfsFsSnapshotOpsV2 *p=(const IfsFsSnapshotOpsV2 *)g;
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
    const IfsFsModuleApiV2 *api, const char *expected_id)
{
    uint32_t id;
    const IfsFsCoreOpsV2 *core;
    if (!api || api->abi_version != IFS_FS_COMMON_ABI_V2 ||
        api->structure_bytes < sizeof(*api) ||
        !ifs_fs_format_id_equal(api->format_id, expected_id) ||
        !api->runtime_profiles ||
        (api->runtime_profiles & ~IFS_FS_PROFILE_ALL) ||
        (api->group_mask & ~IFS_FS_GROUP_ALL) ||
        !(api->group_mask & IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)) ||
        !api->query_group)
        return 0;
    core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    if (!core || !(core->header.capabilities & IFS_FS_CORE_PROBE))
        return 0;
    for (id=1u;id<=IFS_FS_GROUP_MAX;++id)
        if ((api->group_mask & IFS_FS_GROUP_BIT(id)) &&
            !ifs_fs_group_v2_callbacks_valid(ifs_fs_group_v2(api,id)))
            return 0;

    /* All mounted-volume groups require the CORE lifecycle. VOLUME_FORMAT
     * alone operates on an exclusive host and needs no mounted namespace. */
    if (api->group_mask &
        (IFS_FS_GROUP_BIT(IFS_FS_GROUP_HANDLES) |
         IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA) |
         IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG) |
         IFS_FS_GROUP_BIT(IFS_FS_GROUP_STREAMS) |
         IFS_FS_GROUP_BIT(IFS_FS_GROUP_SNAPSHOTS))) {
        if (!(core->header.capabilities & IFS_FS_CORE_MOUNT))
            return 0;
    }
    if (api->group_mask & IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)) {
        const IfsFsVolumeOpsV2 *volume_ops =
            (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
        if ((volume_ops->header.capabilities & ~IFS_FS_VOLUME_FORMAT) &&
            !(core->header.capabilities & IFS_FS_CORE_MOUNT))
            return 0;
    }
    return 1;
}
#ifdef __cplusplus
}
#endif
#endif
