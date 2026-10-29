/* SPDX-License-Identifier: GPL-3.0-or-later
 * V1/V2 compatibility adapters over the same portable OFS engine as V3/Boot.
 * Public layouts and the original 512-byte DOS0/2/4 contract stay unchanged.
 * The old standalone EFI reader is no longer linked into OFS.so.
 */
#include "ofs_module.h"
#include "../core/ofs_engine.h"
static OfsFs legacy;
static int block_read(void *opaque, uint32_t block, uint8_t data[512]) {
    const IfsOfsDropInIo *io = opaque;
    uint64_t at = (uint64_t)block * 512u;
    if (!io || !io->read_at || at > io->volume_bytes || 512u > io->volume_bytes - at)
        return -1;
    return io->read_at(io->context, at, data, 512u) == 0 ? 0 : -1;
}
static uint32_t flags(uint32_t variant) { return variant == 0 ? 0 : variant == 2 ? 2u : 6u; }
static int bind(const IfsOfsDropInVolume *v) {
    if (!v || !v->io.read_at || v->reader.opaque != &v->io || v->reader.read_block != block_read)
        return -1;
    ofs_release(&legacy);
    IfsOfsIoV3 io = {
        sizeof(io), IFS_OFS_READ_ONLY, v->io.context, v->io.volume_bytes, v->io.read_at, 0, 0, 0, 0,
        0};
    IfsOfsMountV3 options = {sizeof(options), 512u, 2u, 0};
    int r = ofs_probe(&legacy, &io, &options);
    return r || legacy.variant > 4u || legacy.root != v->reader.root ||
                   legacy.blocks != v->reader.blocks ||
                   flags(legacy.variant) != v->reader.variant_flags
               ? -1
               : 0;
}
static int mount_volume(IfsOfsDropInVolume *v, const IfsOfsDropInIo *io) {
    if (!v || !io || !io->read_at)
        return -1;
    ofs_release(&legacy);
    v->io = *io;
    IfsOfsIoV3 input = {sizeof(input),
                        IFS_OFS_READ_ONLY,
                        io->context,
                        io->volume_bytes,
                        io->read_at,
                        0,
                        0,
                        0,
                        0,
                        0};
    IfsOfsMountV3 options = {sizeof(options), 512u, 2u, 0};
    if (ofs_probe(&legacy, &input, &options) || legacy.variant > 4u)
        return -1;
    v->reader =
        (IfsOfsEfiVolume){block_read, &v->io, legacy.blocks, legacy.root, flags(legacy.variant)};
    return 0;
}
static IfsOfsEfiEntry root(const IfsOfsDropInVolume *v) {
    return (IfsOfsEfiEntry){v ? v->reader.root : 0, 0, IFS_OFS_EFI_DIRECTORY};
}
static int entry(const OfsNode *n, IfsOfsEfiEntry *out) {
    if (!out ||
        (n->type != IFS_OFS_ROOT && n->type != IFS_OFS_DIRECTORY && n->type != IFS_OFS_FILE))
        return -1;
    *out = (IfsOfsEfiEntry){n->key, n->bytes,
                            n->type == IFS_OFS_FILE ? IFS_OFS_EFI_FILE : IFS_OFS_EFI_DIRECTORY};
    return 0;
}
static int lookup(const IfsOfsDropInVolume *v, const IfsOfsEfiEntry *dir, const uint8_t *name,
                  uint32_t length, IfsOfsEfiEntry *out) {
    if (bind(v) || !dir || dir->type != IFS_OFS_EFI_DIRECTORY || ofs_name_valid(name, length, 30u))
        return -1;
    OfsNode n;
    int r = ofs_lookup(&legacy, dir->block, name, length, &n);
    return r == IFS_OFS_NOT_FOUND ? 1 : r ? -1 : entry(&n, out);
}
static int next(const IfsOfsDropInVolume *v, const IfsOfsEfiEntry *dir, IfsOfsEfiDirCursor *cursor,
                IfsOfsEfiEntry *out, uint8_t name[31]) {
    if (bind(v) || !dir || dir->type != IFS_OFS_EFI_DIRECTORY || !cursor || !name ||
        cursor->visited >= IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES)
        return -1;
    OfsDirCursor c = {cursor->bucket, cursor->next_block, cursor->next_block, cursor->visited};
    OfsNode n;
    int r = ofs_next(&legacy, dir->block, &c, &n);
    if (r)
        return r == IFS_OFS_END ? 1 : -1;
    if (entry(&n, out))
        return -1;
    ofs_copy(name, n.name, n.name_bytes);
    name[n.name_bytes] = 0;
    *cursor = (IfsOfsEfiDirCursor){c.bucket, c.next, c.visited};
    return 0;
}
static int file_node(const IfsOfsEfiEntry *file, OfsNode *n) {
    if (!file || file->type != IFS_OFS_EFI_FILE ||
        ofs_node(&legacy, file->block, n, legacy.scratch[1]) || n->type != IFS_OFS_FILE ||
        n->bytes != file->size)
        return -1;
    return 0;
}
static int read_file(const IfsOfsDropInVolume *v, const IfsOfsEfiEntry *file, uint32_t at,
                     void *buffer, uint32_t count, IfsOfsEfiFileCursor *cursor) {
    OfsNode n;
    if (bind(v) || file_node(file, &n))
        return -1;
    OfsFileCursor c = {0};
    int r = ofs_read_file(&legacy, &n, at, buffer, count, &c);
    if (!r && cursor) {
        cursor->sequence = c.sequence;
        cursor->block = c.block;
    }
    return r ? -1 : 0;
}
static int overwrite(IfsOfsDropInVolume *v, const IfsOfsEfiEntry *file, uint32_t at,
                     const void *source, uint32_t count, IfsOfsWriteAt write_at, IfsOfsSync sync) {
    OfsNode n;
    if (bind(v) || file_node(file, &n) || !write_at || !sync || !source || !count ||
        at >= n.bytes || count > n.bytes - at)
        return -1;
    if (ofs_check_file(&legacy, &n))
        return -1;
    /* V2 retains its existing-data callback authority and persistence checks.
     * V3 owns normal dirty-bitmap/metadata operations and allocation. */
    legacy.io.flags = 0;
    legacy.io.write_at = write_at;
    legacy.io.flush = sync;
    const uint8_t *input = source;
    uint32_t done = 0, total = (n.bytes - 1u) / 488u + 1u;
    OfsFileCursor c = {0};
    while (done < count) {
        uint32_t position = at + done, logical = position / 488u, key;
        if (ofs_pointer(&legacy, n.object, logical, total, &c, &key))
            return -1;
        uint8_t *b = legacy.scratch[4], *persisted = legacy.scratch[5];
        if (ofs_data(&legacy, n.object, n.bytes, logical, key, b))
            return -1;
        uint32_t part = 488u - position % 488u;
        if (part > count - done)
            part = count - done;
        ofs_copy(b + 24u + position % 488u, input + done, part);
        ofs_checksum(&legacy, b, 5);
        if (ofs_write_block(&legacy, key, b) || sync(v->io.context) ||
            v->io.read_at(v->io.context, (uint64_t)key * 512u, persisted, 512u) ||
            !ofs_equal(b, persisted, 512u))
            return -1;
        done += part;
    }
    return 0;
}
__attribute__((used, section(".ifsmeta"))) static const char identity[] = "IFS-MODULE:ofs:1";
__attribute__((used, section(".ifsmeta"))) static const char identity2[] = "IFS-MODULE:ofs:2";
static const IfsOfsDropInModule module = {
    1,
    sizeof(module),
    "ofs",
    "Amiga Original File System",
    IFS_OFS_CAP_PROBE | IFS_OFS_CAP_LOOKUP | IFS_OFS_CAP_ENUMERATE | IFS_OFS_CAP_FILE_READ |
        IFS_OFS_CAP_DOS0 | IFS_OFS_CAP_DOS2 | IFS_OFS_CAP_DOS4 | IFS_OFS_CAP_BLOCK512,
    mount_volume,
    root,
    lookup,
    next,
    read_file};
static const IfsOfsDropInModuleV2 module2 = {2, sizeof(module2), "ofs",
                                             IFS_OFS_CAP_EXISTING_FILE_OVERWRITE, overwrite};
__attribute__((visibility("default"))) const IfsOfsDropInModule *
infiltrator_filesystem_module_v1(void) {
    return &module;
}
__attribute__((visibility("default"))) const IfsOfsDropInModuleV2 *
infiltrator_filesystem_module_v2(void) {
    return &module2;
}
