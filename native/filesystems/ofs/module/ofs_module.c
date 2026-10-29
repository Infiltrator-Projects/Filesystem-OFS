/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Single-file OFS.so adapter of the already-qualified OFS EFI reader,
 * shared canonical OFS primitives and DOS type classification.
 * No EFI protocol or Linux kernel symbols are imported.
 */
#include "ofs_module.h"
#include "../core/ofs_primitives.h"

static int block_read(void *opaque, uint32_t block, uint8_t data[512])
{
    const IfsOfsDropInIo *io = (const IfsOfsDropInIo *)opaque;
    const uint64_t offset = (uint64_t)block * UINT64_C(512);
    if (!io || !io->read_at || offset > io->volume_bytes ||
        UINT64_C(512) > io->volume_bytes - offset)
        return -1;
    return io->read_at(io->context, offset, data, 512u) == 0 ? 0 : -1;
}
static int module_mount(IfsOfsDropInVolume *volume,
                        const IfsOfsDropInIo *io)
{
    uint64_t blocks;
    if (!volume || !io || !io->read_at || io->volume_bytes % 512u ||
        io->volume_bytes < 16u * UINT64_C(512))
        return -1;
    blocks = io->volume_bytes / 512u;
    if (blocks > UINT32_MAX)
        return -1;
    volume->io = *io;
    return ifs_ofs_efi_mount(&volume->reader, (uint32_t)blocks,
                             block_read, &volume->io);
}
static IfsOfsEfiEntry module_root(const IfsOfsDropInVolume *v)
{
    return ifs_ofs_efi_root(v ? &v->reader : 0);
}
static int module_lookup(const IfsOfsDropInVolume *v,
                         const IfsOfsEfiEntry *dir,
                         const uint8_t *name, uint32_t length,
                         IfsOfsEfiEntry *out)
{
    return v ? ifs_ofs_efi_lookup(&v->reader, dir, name, length, out) : -1;
}
static int module_next(const IfsOfsDropInVolume *v,
                       const IfsOfsEfiEntry *dir, IfsOfsEfiDirCursor *cursor,
                       IfsOfsEfiEntry *out, uint8_t name[31])
{
    return v ? ifs_ofs_efi_next(&v->reader, dir, cursor, out, name) : -1;
}
static int module_read(const IfsOfsDropInVolume *v,
                       const IfsOfsEfiEntry *file, uint32_t offset,
                       void *buffer, uint32_t length,
                       IfsOfsEfiFileCursor *cursor)
{
    return v ? ifs_ofs_efi_read(&v->reader, file, offset, buffer, length,
                                cursor) : -1;
}
/* Non-executed format identity for safe single-file selection. */
__attribute__((used,section(".ifsmeta")))
static const char ifs_module_format_identity[] = "IFS-MODULE:ofs:1";
static const IfsOfsDropInModule module = {
    IFS_OFS_DROPIN_ABI_VERSION,
    sizeof(IfsOfsDropInModule),
    "ofs",
    "Amiga Original File System",
    IFS_OFS_CAP_PROBE | IFS_OFS_CAP_LOOKUP |
    IFS_OFS_CAP_ENUMERATE | IFS_OFS_CAP_FILE_READ |
    IFS_OFS_CAP_DOS0 | IFS_OFS_CAP_DOS2 |
    IFS_OFS_CAP_DOS4 | IFS_OFS_CAP_BLOCK512,
    module_mount, module_root, module_lookup, module_next, module_read
};
__attribute__((visibility("default")))
const IfsOfsDropInModule *infiltrator_filesystem_module_v1(void)
{
    return &module;
}

/* First migrated OFS writer operation: existing allocated file bytes only.
 * The canonical reader locates/checks the data chain; canonical checksum
 * primitives produce the OFS data-block checksum. No bitmap or size change. */
static int module_overwrite_existing(IfsOfsDropInVolume *volume,
    const IfsOfsEfiEntry *file, uint32_t position, const void *source,
    uint32_t length, IfsOfsWriteAt write_at, IfsOfsSync sync)
{
    IfsOfsEfiVolume refreshed = {0};
    uint8_t block[512], persisted[512];
    uint32_t block_number, expected_sum, done = 0U;
    const uint8_t *data = (const uint8_t *)source;
    if (!volume || !file || !source || !write_at || !sync ||
        !volume->io.read_at || file->type != IFS_OFS_EFI_FILE ||
        !length || position >= file->size ||
        length > file->size - position)
        return -1;
    /* Re-detect the same media identity before permitting any mutation. */
    if (volume->reader.opaque != &volume->io ||
        volume->reader.read_block != block_read ||
        ifs_ofs_efi_mount(&refreshed, volume->reader.blocks,
                          block_read, &volume->io) != 0 ||
        refreshed.root != volume->reader.root ||
        refreshed.variant_flags != volume->reader.variant_flags)
        return -1;
    /* Validate the *entire* destination chain first. An invalid trailing
     * block must never cause an earlier valid block to be overwritten. */
    for (uint32_t checked = 0U; checked < length;) {
        uint32_t at = position + checked;
        uint32_t count = 488U - at % 488U;
        uint64_t offset;
        if (count > length - checked) count = length - checked;
        if (ifs_ofs_efi_resolve_file_block(&volume->reader, file, at,
                                           &block_number, block) != 0)
            return -1;
        offset = (uint64_t)block_number * UINT64_C(512);
        if (offset > volume->io.volume_bytes ||
            UINT64_C(512) > volume->io.volume_bytes - offset)
            return -1;
        checked += count;
    }
    /* Commit each validated data block. The host must hold the volume
     * exclusively; I/O failure may leave already-written blocks modified.
     * There is no implied transaction or crash-atomicity guarantee. */
    while (done < length) {
        uint32_t at = position + done;
        uint32_t count = 488U - at % 488U;
        uint64_t device_offset;
        if (count > length - done) count = length - done;
        if (ifs_ofs_efi_resolve_file_block(&volume->reader, file, at,
                                           &block_number, block) != 0)
            return -1;
        device_offset = (uint64_t)block_number * UINT64_C(512);
        if (device_offset > volume->io.volume_bytes ||
            UINT64_C(512) > volume->io.volume_bytes - device_offset)
            return -1;
        for (uint32_t i = 0; i < count; ++i)
            block[24U + at % 488U + i] = data[done + i];
        expected_sum = ifs_ofs_checksum_word_value(block, 512U, 5U);
        block[20] = (uint8_t)(expected_sum >> 24);
        block[21] = (uint8_t)(expected_sum >> 16);
        block[22] = (uint8_t)(expected_sum >> 8);
        block[23] = (uint8_t)expected_sum;
        if (ifs_ofs_block_checksum(block, 512U) != 0U ||
            write_at(volume->io.context, device_offset, block, 512U) != 0 ||
            sync(volume->io.context) != 0 ||
            volume->io.read_at(volume->io.context, device_offset,
                               persisted, 512U) != 0 ||
            ifs_ofs_block_checksum(persisted, 512U) != 0U)
            return -1;
        for (uint32_t i = 0; i < 512U; ++i)
            if (persisted[i] != block[i]) return -1;
        done += count;
    }
    return 0;
}
__attribute__((used,section(".ifsmeta")))
static const char ifs_module_format_identity_v2[] = "IFS-MODULE:ofs:2";
static const IfsOfsDropInModuleV2 module_v2 = {
    IFS_OFS_DROPIN_ABI_V2, sizeof(IfsOfsDropInModuleV2),
    "ofs", IFS_OFS_CAP_EXISTING_FILE_OVERWRITE,
    module_overwrite_existing
};
__attribute__((visibility("default")))
const IfsOfsDropInModuleV2 *infiltrator_filesystem_module_v2(void)
{
    return &module_v2;
}
