/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATR_OFS_EFI_READER_H
#define INFILTRATR_OFS_EFI_READER_H

#include <stddef.h>
#include <stdint.h>

/* Pure, read-only 512-byte AmigaDOS DOS\0 filesystem access.
 * Firmware owns block I/O, allocation and protocol lifetime.
 * The callback receives a partition-relative 512-byte block number.
 */
typedef int (*ifs_ofs_efi_read_block)(void *opaque, uint32_t block,
                                      uint8_t data[512]);

enum { IFS_OFS_EFI_FILE = 1, IFS_OFS_EFI_DIRECTORY = 2 };

/* Boot-stage directory traversal is independent of volume size: a corrupt
 * hash-chain cycle must not demand billions of firmware reads. An overlong
 * directory fails closed in the EFI reader without changing the disk format. */
#define IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES 512U

typedef struct {
    ifs_ofs_efi_read_block read_block;
    void *opaque;
    uint32_t blocks;
    uint32_t root;
    uint32_t variant_flags; /* DOS\0 / DOS\2 / DOS\4, metadata read-only */
} IfsOfsEfiVolume;

typedef struct {
    uint32_t block;
    uint32_t size;
    int type;
} IfsOfsEfiEntry;

/* Cursor starts all-zero; caller preserves it between calls. */
typedef struct {
    uint32_t bucket;
    uint32_t next_block;
    uint32_t visited;
} IfsOfsEfiDirCursor;

/* Sequential file reads amortise to O(n) using this optional cursor. */
typedef struct {
    uint32_t sequence;
    uint32_t block;
} IfsOfsEfiFileCursor;

/* All functions return 0 on success, -1 on damaged/unsupported I/O,
 * and 1 for a completed enumeration or an absent name.
 */
int ifs_ofs_efi_mount(IfsOfsEfiVolume *volume, uint32_t blocks,
                       ifs_ofs_efi_read_block read_block, void *opaque);
IfsOfsEfiEntry ifs_ofs_efi_root(const IfsOfsEfiVolume *volume);
int ifs_ofs_efi_lookup(const IfsOfsEfiVolume *volume,
                       const IfsOfsEfiEntry *directory,
                       const uint8_t *name, size_t name_length,
                       IfsOfsEfiEntry *result);
int ifs_ofs_efi_next(const IfsOfsEfiVolume *volume,
                     const IfsOfsEfiEntry *directory,
                     IfsOfsEfiDirCursor *cursor,
                     IfsOfsEfiEntry *result, uint8_t name[31]);
int ifs_ofs_efi_read(const IfsOfsEfiVolume *volume,
                     const IfsOfsEfiEntry *file, uint32_t offset,
                     void *buffer, size_t length, IfsOfsEfiFileCursor *cursor);
/* Shared canonical chain resolver: validates the complete traversed OFS data
 * chain and returns one checked 512-byte block for freestanding consumers.
 * No writes, allocations or platform-specific memory services occur here. */
int ifs_ofs_efi_resolve_file_block(const IfsOfsEfiVolume *volume,
                                   const IfsOfsEfiEntry *file,
                                   uint32_t offset, uint32_t *block_number,
                                   uint8_t block_contents[512]);
#endif
