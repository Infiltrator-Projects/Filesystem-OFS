/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone read-only AmigaDOS OFS module ABI.
 * One self-contained OFS.so; no glibc, kernel, or UEFI imports.
 * The volume and its embedded host callback must stay at a stable address.
 */
#ifndef INFILTRATOR_OFS_DROPIN_H
#define INFILTRATOR_OFS_DROPIN_H

#include <stdint.h>
#include "../efi/reader.h"

#define IFS_OFS_DROPIN_ABI_VERSION 1u
#define IFS_OFS_CAP_PROBE UINT64_C(1)
#define IFS_OFS_CAP_LOOKUP UINT64_C(2)
#define IFS_OFS_CAP_ENUMERATE UINT64_C(4)
#define IFS_OFS_CAP_FILE_READ UINT64_C(8)
#define IFS_OFS_CAP_DOS0 UINT64_C(16)
#define IFS_OFS_CAP_DOS2 UINT64_C(32)
#define IFS_OFS_CAP_DOS4 UINT64_C(64)
#define IFS_OFS_CAP_BLOCK512 UINT64_C(128)

typedef int (*IfsOfsReadAt)(void *context, uint64_t offset,
                             void *buffer, uint32_t bytes);

typedef struct {
    IfsOfsReadAt read_at; /* zero on complete read, nonzero on failure */
    void *context;
    uint64_t volume_bytes; /* partition-relative, never whole-disk */
} IfsOfsDropInIo;

typedef struct {
    IfsOfsDropInIo io;
    IfsOfsEfiVolume reader;
} IfsOfsDropInVolume;

typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    const char *format_name;
    uint64_t capabilities;
    int (*mount)(IfsOfsDropInVolume *, const IfsOfsDropInIo *);
    IfsOfsEfiEntry (*root)(const IfsOfsDropInVolume *);
    int (*lookup)(const IfsOfsDropInVolume *, const IfsOfsEfiEntry *,
                  const uint8_t *name, uint32_t name_length,
                  IfsOfsEfiEntry *);
    int (*next)(const IfsOfsDropInVolume *, const IfsOfsEfiEntry *,
                IfsOfsEfiDirCursor *, IfsOfsEfiEntry *, uint8_t name[31]);
    int (*read)(const IfsOfsDropInVolume *, const IfsOfsEfiEntry *,
                uint32_t offset, void *buffer, uint32_t length,
                IfsOfsEfiFileCursor *);
} IfsOfsDropInModule;

/* V1 supports read-only 512-byte DOS\0/\2/\4 only, NO formatter,
 * write, defragmentation, or Linux VFS / UEFI module loading yet. */
const IfsOfsDropInModule *infiltrator_filesystem_module_v1(void);
/* V2 extends the V1 *contract*; no V1 layout or pointer is changed.
 * Explicit write and sync capabilities come from the privileged host.
 * Only bounded existing-file bytes are writable until allocation and
 * transactional metadata mutation are independently qualified. */
#define IFS_OFS_DROPIN_ABI_V2 2u
#define IFS_OFS_CAP_EXISTING_FILE_OVERWRITE UINT64_C(256)
typedef int (*IfsOfsWriteAt)(void *context, uint64_t offset,
                             const void *buffer, uint32_t bytes);
typedef int (*IfsOfsSync)(void *context);
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    /* Returns 0 on verified success, -1 for unsupported, corrupt or I/O
     * error. Exactly one OFS data block per operation. Cannot grow a file.
     * The write callback is *always bound to the same host context* as
     * the mount's read callback; it cannot target a second device. */
    int (*overwrite_existing)(IfsOfsDropInVolume *volume,
        const IfsOfsEfiEntry *file, uint32_t offset, const void *bytes,
        uint32_t length, IfsOfsWriteAt write_at, IfsOfsSync sync);
} IfsOfsDropInModuleV2;
const IfsOfsDropInModuleV2 *infiltrator_filesystem_module_v2(void);
#endif
