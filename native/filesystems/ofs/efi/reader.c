/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Canonical OFS core + freestanding, bounded, read-only boot access. */
#include "reader.h"
#include "../core/ofs_core.h"
#include "../core/ofs_primitives.h"

enum {
    OFS_BLOCK = 512, OFS_DATA = 488, OFS_HASH = 72,
    OFS_ROOT = 1, OFS_DIR = 2, OFS_FILE = -3,
    OFS_META_TYPE = 2, OFS_DATA_TYPE = 8
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t word(const uint8_t *p, uint32_t n)
{
    return be32(p + n * 4U);
}
static int valid(const IfsOfsEfiVolume *v, uint32_t block)
{
    return v && block > 1U && block < v->blocks;
}
static int get(const IfsOfsEfiVolume *v, uint32_t block, uint8_t b[512])
{
    return valid(v, block) && v->read_block(v->opaque, block, b) == 0 ? 0 : -1;
}
static int metadata(const IfsOfsEfiVolume *v, uint32_t block,
                    uint8_t b[512])
{
    if (get(v, block, b) != 0 || word(b, 0) != OFS_META_TYPE ||
        ifs_ofs_block_checksum(b, OFS_BLOCK) != 0U)
        return -1;
    return 0;
}
static int name_valid(const uint8_t b[512])
{
    const uint32_t length = b[432];
    return length > 0U &&
           ifs_ofs_validate_name(b + 433, length, 1) == IFS_OFS_NAME_OK;
}
static int entry(const IfsOfsEfiVolume *v, uint32_t block,
                 uint32_t parent, uint8_t b[512], IfsOfsEfiEntry *out)
{
    uint32_t kind;
    if (!out || metadata(v, block, b) != 0 ||
        word(b, 1) != block || word(b, 125) != parent ||
        !name_valid(b))
        return -1;
    kind = word(b, 127);
    if (kind != (uint32_t)OFS_FILE && kind != (uint32_t)OFS_DIR)
        return -1; /* Links, caches and special entries are not boot sources. */
    out->block = block;
    out->type = kind == (uint32_t)OFS_DIR
                    ? IFS_OFS_EFI_DIRECTORY : IFS_OFS_EFI_FILE;
    out->size = out->type == IFS_OFS_EFI_FILE ? word(b, 81) : 0U;
    return 0;
}
static int equal_name(const IfsOfsEfiVolume *v, const uint8_t b[512],
                      const uint8_t *needle, size_t length)
{
    const int intl = (v->variant_flags & IFS_OFS_VARIANT_INTL) != 0U;
    size_t i;
    if (b[432] != length)
        return 0;
    for (i = 0U; i < length; ++i)
        if (ifs_ofs_fold_character(b[433 + i], intl) !=
            ifs_ofs_fold_character(needle[i], intl))
            return 0;
    return 1;
}
static int directory(const IfsOfsEfiVolume *v,
                     const IfsOfsEfiEntry *dir, uint8_t b[512])
{
    if (!dir || dir->type != IFS_OFS_EFI_DIRECTORY ||
        metadata(v, dir->block, b) != 0)
        return -1;
    if (dir->block == v->root)
        return word(b, 127) == OFS_ROOT && word(b, 3) == OFS_HASH ? 0 : -1;
    return word(b, 1) == dir->block && word(b, 127) == OFS_DIR ? 0 : -1;
}
int ifs_ofs_efi_mount(IfsOfsEfiVolume *v, uint32_t blocks,
                       ifs_ofs_efi_read_block callback, void *opaque)
{
    uint8_t b[OFS_BLOCK];
    uint32_t flags = 0;
    if (!v || !callback || blocks < 16U)
        return -1;
    v->read_block = callback;
    v->opaque = opaque;
    v->blocks = blocks;
    if (ifs_ofs_root_block(2U, blocks, &v->root) != 0)
        return -1;
    if (callback(opaque, 0U, b) != 0 ||
        ifs_ofs_classify_dostype(word(b, 0), &flags) != 0 ||
        (flags & IFS_OFS_VARIANT_MUFS) != 0U ||
        directory(v, &(IfsOfsEfiEntry){v->root, 0U,
                   IFS_OFS_EFI_DIRECTORY}, b) != 0 ||
        b[432] > 30U)
        return -1;
    v->variant_flags = flags;
    return 0;
}
IfsOfsEfiEntry ifs_ofs_efi_root(const IfsOfsEfiVolume *v)
{
    IfsOfsEfiEntry root = {0U, 0U, IFS_OFS_EFI_DIRECTORY};
    if (v)
        root.block = v->root;
    return root;
}
int ifs_ofs_efi_lookup(const IfsOfsEfiVolume *v,
                       const IfsOfsEfiEntry *dir,
                       const uint8_t *name, size_t length,
                       IfsOfsEfiEntry *out)
{
    uint8_t b[OFS_BLOCK];
    uint32_t block, visited = 0U, bucket;
    if (!v || !name || !out || length == 0U || length > 30U ||
        ifs_ofs_validate_name(name, (uint32_t)length, 1) != IFS_OFS_NAME_OK ||
        directory(v, dir, b) != 0)
        return -1;
    bucket = ifs_ofs_directory_hash(name, (uint32_t)length, OFS_HASH,
                                    (v->variant_flags & IFS_OFS_VARIANT_INTL) != 0U);
    block = word(b, 6U + bucket);
    while (block != 0U) {
        uint32_t next;
        if (++visited > v->blocks ||
            visited > IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES ||
            entry(v, block, dir->block, b, out) != 0 ||
            ifs_ofs_directory_hash(b + 433, b[432], OFS_HASH,
                                       (v->variant_flags & IFS_OFS_VARIANT_INTL) != 0U) != bucket)
            return -1;
        next = word(b, 124U);
        if (equal_name(v, b, name, length))
            return 0;
        block = next;
    }
    return 1;
}
int ifs_ofs_efi_next(const IfsOfsEfiVolume *v,
                     const IfsOfsEfiEntry *dir,
                     IfsOfsEfiDirCursor *cursor,
                     IfsOfsEfiEntry *out, uint8_t name[31])
{
    uint8_t b[OFS_BLOCK], dirb[OFS_BLOCK];
    uint32_t block, i;
    if (!v || !cursor || !out || !name || cursor->bucket > OFS_HASH ||
        cursor->visited > v->blocks ||
        cursor->visited > IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES ||
        directory(v, dir, dirb) != 0)
        return -1;
    while (cursor->bucket < OFS_HASH) {
        block = cursor->next_block;
        if (block == 0U)
            block = word(dirb, 6U + cursor->bucket);
        if (block == 0U) {
            ++cursor->bucket;
            continue;
        }
        if (++cursor->visited > v->blocks ||
            cursor->visited > IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES ||
            entry(v, block, dir->block, b, out) != 0 ||
            ifs_ofs_directory_hash(b + 433, b[432], OFS_HASH,
                                        (v->variant_flags & IFS_OFS_VARIANT_INTL) != 0U) !=
                cursor->bucket)
            return -1;
        for (i = 0U; i < b[432]; ++i)
            name[i] = b[433 + i];
        name[i] = 0U;
        cursor->next_block = word(b, 124U);
        if (cursor->next_block == 0U)
            ++cursor->bucket;
        return 0;
    }
    return 1;
}
static int data(const IfsOfsEfiVolume *v, const IfsOfsEfiEntry *file,
                uint32_t block, uint32_t sequence, uint8_t b[512])
{
    const uint32_t size = file->size -
        (sequence - 1U) * OFS_DATA;
    const uint32_t expected = size < OFS_DATA ? size : OFS_DATA;
    if (get(v, block, b) != 0 || word(b, 0) != OFS_DATA_TYPE ||
        word(b, 1) != file->block || word(b, 2) != sequence ||
        word(b, 3) != expected || ifs_ofs_block_checksum(b, OFS_BLOCK) != 0U)
        return -1;
    if (size > OFS_DATA && !valid(v, word(b, 4)))
        return -1;
    return 0;
}
int ifs_ofs_efi_read(const IfsOfsEfiVolume *v,
                     const IfsOfsEfiEntry *file, uint32_t offset,
                     void *buffer, size_t length, IfsOfsEfiFileCursor *cursor)
{
    uint8_t b[OFS_BLOCK];
    uint32_t seq, block, index, within;
    uint8_t *dst = (uint8_t *)buffer;
    size_t remaining = length;
    if (!v || !file || file->type != IFS_OFS_EFI_FILE ||
        (!dst && remaining != 0U) || offset > file->size ||
        remaining > (size_t)(file->size - offset))
        return -1;
    if (remaining == 0U)
        return 0;
    if (metadata(v, file->block, b) != 0 ||
        word(b, 1) != file->block || word(b, 127) != (uint32_t)OFS_FILE ||
        word(b, 81) != file->size)
        return -1;
    block = word(b, 4);
    seq = 1U;
    index = offset / OFS_DATA + 1U;
    if (cursor && cursor->sequence > 0U && cursor->sequence <= index &&
        cursor->sequence <= (file->size - 1U) / OFS_DATA + 1U &&
        valid(v, cursor->block)) {
        seq = cursor->sequence;
        block = cursor->block;
    }
    while (seq < index) {
        if (data(v, file, block, seq, b) != 0)
            return -1;
        block = word(b, 4);
        ++seq;
    }
    within = offset % OFS_DATA;
    while (remaining != 0U) {
        size_t n, i;
        if (data(v, file, block, seq, b) != 0)
            return -1;
        if (cursor) {
            cursor->sequence = seq;
            cursor->block = block;
        }
        n = OFS_DATA - within;
        if (n > remaining)
            n = remaining;
        for (i = 0U; i < n; ++i)
            *dst++ = b[24U + within + i];
        remaining -= n;
        block = word(b, 4);
        ++seq;
        within = 0U;
    }
    return 0;
}

/* Central data-chain resolver used by the reader and the future portable
 * OFS writer. This is the same validated chain, not a duplicate parser. */
int ifs_ofs_efi_resolve_file_block(const IfsOfsEfiVolume *v,
                                   const IfsOfsEfiEntry *file,
                                   uint32_t offset, uint32_t *block_number,
                                   uint8_t contents[512])
{
    uint8_t header[512];
    uint32_t current, seq, wanted;
    if (!v || !file || !block_number || !contents ||
        file->type != IFS_OFS_EFI_FILE || offset >= file->size ||
        metadata(v, file->block, header) != 0 ||
        word(header, 1) != file->block ||
        word(header, 127) != (uint32_t)OFS_FILE ||
        word(header, 81) != file->size)
        return -1;
    current = word(header, 4);
    wanted = offset / OFS_DATA + 1U;
    for (seq = 1U; seq <= wanted; ++seq) {
        if (data(v, file, current, seq, contents) != 0)
            return -1;
        if (seq == wanted) {
            *block_number = current;
            return 0;
        }
        current = word(contents, 4);
    }
    return -1;
}
