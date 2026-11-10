/* SPDX-License-Identifier: GPL-3.0-or-later
 * Infiltratr Common: format-neutral filesystem module ABI, revision 1.
 *
 * This is a contract for NEW adopters, not a reinterpretation of existing
 * infiltrator_filesystem_module_vN() format-specific tables.
 * No runtime library imports; usable by freestanding host adapters.
 */
#ifndef INFILTRATR_FILESYSTEM_MODULE_H
#define INFILTRATR_FILESYSTEM_MODULE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define IFS_FS_COMMON_ABI_V1 1u
#define IFS_FS_COMMON_EXPORT_V1 "infiltrator_filesystem_api_v1"
#define IFS_FS_MAX_FORMAT_ID 63u

/* These bits have the SAME meaning for every format. Do not copy a
 * format-specific IFS_*_CAP_* mask into this field. */
#define IFS_FS_CAP_PROBE        (UINT64_C(1) << 0)
#define IFS_FS_CAP_MOUNT        (UINT64_C(1) << 1)
#define IFS_FS_CAP_LIST         (UINT64_C(1) << 2)
#define IFS_FS_CAP_READ         (UINT64_C(1) << 3)
#define IFS_FS_CAP_WRITE        (UINT64_C(1) << 4)
#define IFS_FS_CAP_CREATE       (UINT64_C(1) << 5)
#define IFS_FS_CAP_REMOVE       (UINT64_C(1) << 6)
#define IFS_FS_CAP_RENAME       (UINT64_C(1) << 7)
#define IFS_FS_CAP_TRUNCATE     (UINT64_C(1) << 8)
#define IFS_FS_CAP_FORMAT       (UINT64_C(1) << 9)
#define IFS_FS_CAP_CHECK        (UINT64_C(1) << 10)
#define IFS_FS_CAP_RECOVER      (UINT64_C(1) << 11)
#define IFS_FS_CAP_DEFRAG_INFO  (UINT64_C(1) << 12)
#define IFS_FS_CAP_DEFRAG_MOVE  (UINT64_C(1) << 13)

#define IFS_FS_IO_READ_ONLY 1u
#define IFS_FS_IO_EXCLUSIVE 2u /* host has verified exclusive access */
#define IFS_FS_ENTRY_FILE 1u
#define IFS_FS_ENTRY_DIRECTORY 2u
#define IFS_FS_ENTRY_LINK 3u

/* Statuses are ABI values; no errno, platform-dependent exceptions or
 * silent fallbacks to external executables. */
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

typedef struct {
    uint32_t structure_bytes;
    uint32_t flags;
    void *context;
    uint64_t volume_bytes; /* partition-relative; never a whole disk */
    int (*read_at)(void *, uint64_t, void *, uint32_t);
    int (*write_at)(void *, uint64_t, const void *, uint32_t);
    int (*flush)(void *);
    void *(*allocate)(void *, size_t);
    void (*release)(void *, void *);
} IfsFsHostIoV1;

/* Optional ABI-1 tail for explicit formatting of blank media. No V1 field
 * moves: legacy readers still see the identical IfsFsHostIoV1 prefix.
 * The privileged host obtains the logical sector size from the device or
 * a verified image contract; the filesystem must never guess it. Pass
 * sizeof(IfsFsHostFormatIoV1) as host.structure_bytes and reserved=0.
 * An unsupported format may reject the extension without writing. */
typedef struct {
    IfsFsHostIoV1 host;
    uint32_t logical_sector_bytes;
    uint32_t reserved;
} IfsFsHostFormatIoV1;

typedef struct {
    uint32_t structure_bytes;
    uint32_t type;
    uint64_t size_bytes;
    uint32_t attributes;
    uint32_t name_bytes; /* written byte count, excluding trailing NUL */
    char *name;          /* caller-owned UTF-8 buffer, NUL terminated */
    uint32_t name_capacity;
} IfsFsEntryV1;

typedef struct {
    uint32_t structure_bytes;
    uint32_t variant; /* disk-identified variant; NOT module ABI version */
    uint32_t max_name_bytes; /* on-media limit; zero = unknown */
    uint32_t block_bytes;
    uint64_t total_bytes;
    uint64_t free_bytes;
} IfsFsVolumeInfoV1;

/* All paths are UTF-8, volume-relative; a module must enforce its on-disk
 * variant's own name, size, protection and metadata constraints. The host
 * owns exclusive raw-device / destructive-operation authorisation.
 *
 * NULL operation pointer or missing capability = UNSUPPORTED. Capability
 * flags describe implemented operations, NOT device/firmware qualification.
 * None of these functions executes automatically during discovery.
 */
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*probe)(const IfsFsHostIoV1 *, uint32_t *detected_variant);
    int (*mount)(const IfsFsHostIoV1 *, void **volume,
                 IfsFsVolumeInfoV1 *);
    int (*unmount)(void *volume);
    int (*stat)(void *volume, const char *path, IfsFsEntryV1 *);
    int (*directory_entry)(void *volume, const char *directory,
                           uint32_t index, IfsFsEntryV1 *);
    int (*read)(void *volume, const char *path, uint64_t offset,
                void *destination, uint32_t length);
    int (*write)(void *volume, const char *path, uint64_t offset,
                 const void *source, uint32_t length);
    int (*create)(void *volume, const char *path, uint32_t type);
    int (*remove)(void *volume, const char *path);
    int (*rename)(void *volume, const char *old_path,
                  const char *new_path);
    int (*truncate)(void *volume, const char *path, uint64_t size);
    int (*format)(const IfsFsHostIoV1 *, uint32_t variant,
                  const char *volume_label);
    int (*check)(void *volume, uint32_t *errors);
    int (*recover)(void *volume, uint32_t *repaired);
    /* Defrag is deliberately NOT callable by a generic placeholder:
     * reserved capability bits require a separately reviewed contract. */
} IfsFsModuleApiV1;
typedef const IfsFsModuleApiV1 *(*IfsFsModuleApiEntryV1)(void);

/* FIRST THREE fields of the inspected existing drop-in tables.
 * All currently audited FAT12, FAT16, FAT32, OFS, FFS, exFAT, EXT2,
 * Minix and SCO BFS module_vN tables share this prefix. Their other
 * fields and capability bit positions are NOT interchangeable. */
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
} IfsFsLegacyPrefix;

static inline int ifs_fs_format_id_valid(const char *s)
{
    uint32_t n;
    if (!s) return 0;
    for (n = 0; n <= IFS_FS_MAX_FORMAT_ID; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (!c) return n != 0;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 0;
}
static inline int ifs_fs_format_id_equal(const char *a, const char *b)
{
    uint32_t i;
    if (!ifs_fs_format_id_valid(a) || !ifs_fs_format_id_valid(b))
        return 0;
    for (i = 0; i <= IFS_FS_MAX_FORMAT_ID; ++i) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}

/* Inspect an ALREADY TRUSTED, LOADED existing module's returned table.
 * Does not invoke arbitrary module entry points or assume native cap bits.
 * The host must verify architecture/trust before calling module code.
 */
static inline int ifs_fs_legacy_prefix_valid(const void *descriptor,
                                             uint32_t expected_version,
                                             const char *expected_id)
{
    const IfsFsLegacyPrefix *p = (const IfsFsLegacyPrefix *)descriptor;
    return p && expected_version && p->abi_version == expected_version &&
           p->structure_bytes >= sizeof(IfsFsLegacyPrefix) &&
           ifs_fs_format_id_equal(p->format_id, expected_id);
}

static inline int ifs_fs_common_api_valid(const IfsFsModuleApiV1 *a,
                                         const char *expected_id)
{
    uint64_t c;
    if (!a || a->abi_version != IFS_FS_COMMON_ABI_V1 ||
        a->structure_bytes < sizeof(*a) ||
        !ifs_fs_format_id_equal(a->format_id, expected_id))
        return 0;
    c = a->capabilities;
    if (c & ~(IFS_FS_CAP_PROBE | IFS_FS_CAP_MOUNT | IFS_FS_CAP_LIST |
              IFS_FS_CAP_READ | IFS_FS_CAP_WRITE | IFS_FS_CAP_CREATE |
              IFS_FS_CAP_REMOVE | IFS_FS_CAP_RENAME | IFS_FS_CAP_TRUNCATE |
              IFS_FS_CAP_FORMAT | IFS_FS_CAP_CHECK | IFS_FS_CAP_RECOVER))
        return 0; /* Defrag bits reserved until ABI is specified. */
    if (((c & IFS_FS_CAP_PROBE) != 0) != (a->probe != NULL) ||
        ((c & IFS_FS_CAP_MOUNT) != 0) != (a->mount != NULL) ||
        ((c & IFS_FS_CAP_LIST) != 0) != (a->directory_entry != NULL) ||
        ((c & IFS_FS_CAP_READ) != 0) != (a->read != NULL) ||
        ((c & IFS_FS_CAP_WRITE) != 0) != (a->write != NULL) ||
        ((c & IFS_FS_CAP_CREATE) != 0) != (a->create != NULL) ||
        ((c & IFS_FS_CAP_REMOVE) != 0) != (a->remove != NULL) ||
        ((c & IFS_FS_CAP_RENAME) != 0) != (a->rename != NULL) ||
        ((c & IFS_FS_CAP_TRUNCATE) != 0) != (a->truncate != NULL) ||
        ((c & IFS_FS_CAP_FORMAT) != 0) != (a->format != NULL) ||
        ((c & IFS_FS_CAP_CHECK) != 0) != (a->check != NULL) ||
        ((c & IFS_FS_CAP_RECOVER) != 0) != (a->recover != NULL))
        return 0;
    if ((c & IFS_FS_CAP_MOUNT) && !a->unmount) return 0;
    if (!(c & IFS_FS_CAP_MOUNT) && a->unmount) return 0;
    if (!(c & IFS_FS_CAP_MOUNT) && (c & (IFS_FS_CAP_LIST | IFS_FS_CAP_READ |
            IFS_FS_CAP_WRITE | IFS_FS_CAP_CREATE | IFS_FS_CAP_REMOVE |
            IFS_FS_CAP_RENAME | IFS_FS_CAP_TRUNCATE | IFS_FS_CAP_CHECK |
            IFS_FS_CAP_RECOVER)))
        return 0;
    if ((c & IFS_FS_CAP_MOUNT) && !a->stat) return 0;
    return 1;
}
#ifdef __cplusplus
}
#endif
#endif
