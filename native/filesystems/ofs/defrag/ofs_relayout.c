/* SPDX-License-Identifier: GPL-3.0-or-later
 * OFS_SPECIFICATION sections 7..16. Relocation consumes the canonical engine's
 * verified namespace, data cursors and bitmap index. There is no second OFS
 * parser. All original objects are visited once, including cyclic aliases.
 * A complete stage is built before any source write. System bitmap geometry
 * and root stay fixed; other metadata is packed first, then file payloads.
 */
#include "ofs_relayout.h"

static int bit(const uint8_t *p, uint32_t k) { return (p[k / 8u] >> (k % 8u)) & 1u; }
static void set(uint8_t *p, uint32_t k, int value) {
    if (value) p[k / 8u] |= (uint8_t)(1u << (k % 8u));
    else p[k / 8u] &= (uint8_t)~(1u << (k % 8u));
}
static int stopped(OfsLayout *m) {
    return m->io.host->stop_requested(m->io.host->context) ? IFS_DEFRAG_STOPPED : 0;
}
static int read_io(void *opaque, uint64_t at, void *p, uint32_t n) {
    OfsDefragIo *io = opaque;
    return io->host->read_at(io->host->context, io->target, at, p, n);
}
static void *allocate_io(void *opaque, size_t n) {
    OfsDefragIo *io = opaque;
    return io->host->allocate(n);
}
static void release_io(void *opaque, void *p) {
    OfsDefragIo *io = opaque;
    io->host->release(p);
}
static int reserve(OfsLayout *m, void **p, size_t *cap, size_t count, size_t bytes) {
    if (count <= *cap) return 0;
    size_t next = *cap ? *cap : 32;
    while (next < count) {
        if (next > SIZE_MAX / 2u) return IFS_OFS_NO_MEMORY;
        next *= 2u;
    }
    if (next > SIZE_MAX / bytes) return IFS_OFS_NO_MEMORY;
    void *q = m->io.host->allocate(next * bytes);
    if (!q) return IFS_OFS_NO_MEMORY;
    ofs_zero(q, next * bytes);
    if (*p) { ofs_copy(q, *p, *cap * bytes); m->io.host->release(*p); }
    *p = q; *cap = next;
    return 0;
}
static int push_directory(OfsLayout *m, uint32_t key) {
    void *p = m->directories;
    int r = reserve(m, &p, &m->directory_capacity, m->directory_count + 1u, sizeof(uint32_t));
    if (r) return r;
    m->directories = p; m->directories[m->directory_count++] = key;
    return 0;
}
static int push_file(OfsLayout *m, uint32_t key) {
    void *p = m->files;
    int r = reserve(m, &p, &m->file_capacity, m->file_count + 1u, sizeof(OfsLayoutFile));
    if (r) return r;
    m->files = p; m->files[m->file_count++].key = key;
    return 0;
}
static int system_key(const OfsFs *f, uint32_t key) {
    uint32_t lo = 0, hi = f->system_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (f->system_keys[mid] < key) lo = mid + 1u;
        else hi = mid;
    }
    return lo < f->system_count && f->system_keys[lo] == key;
}
static uint32_t mapped(const OfsLayout *m, uint32_t key) {
    if (!key || system_key(m->fs, key)) return key;
    size_t lo = 0, hi = m->metadata_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (m->metadata[mid].old_key < key) lo = mid + 1u;
        else hi = mid;
    }
    return lo < m->metadata_count && m->metadata[lo].old_key == key
               ? m->metadata[lo].new_key : 0;
}
void ofs_layout_close(OfsLayout *m) {
    if (!m || !m->io.host) return;
    const IfsDefragHostV2 *h = m->io.host;
    for (size_t i = 0; i < m->file_count; ++i) {
        h->release(m->files[i].old_data); h->release(m->files[i].new_data);
    }
    h->release(m->files); h->release(m->metadata); h->release(m->directories);
    h->release(m->used); h->release(m->source_used); h->release(m->occupied); h->release(m->source_data);
    if (m->fs) { ofs_release(m->fs); h->release(m->fs); }
    ofs_zero(m, sizeof(*m));
}
int ofs_layout_open(OfsLayout *m, const IfsDefragHostV2 *h, IfsDefragTarget *target) {
    ofs_zero(m, sizeof(*m)); m->io = (OfsDefragIo){h, target};
    m->fs = h->allocate(sizeof(OfsFs));
    if (!m->fs) return IFS_OFS_NO_MEMORY;
    ofs_zero(m->fs, sizeof(OfsFs));
    IfsOfsIoV3 io = {sizeof(io), IFS_OFS_READ_ONLY, &m->io, target->bytes,
                     read_io, 0, 0, allocate_io, release_io, 0};
    int r = ofs_probe(m->fs, &io, 0);
    IfsOfsCheckV3 check;
    if (!r) r = ofs_verify(m->fs, &check, 0);
    if (r) { ofs_layout_close(m); return r; }
    OfsFs *f = m->fs;
    m->map_bytes = ((size_t)f->blocks + 7u) / 8u;
    m->used = h->allocate(m->map_bytes);
    m->source_used = h->allocate(m->map_bytes);
    m->occupied = h->allocate(m->map_bytes);
    m->source_data = h->allocate(m->map_bytes);
    uint8_t *seen = h->allocate(m->map_bytes);
    if (!m->used || !m->source_used || !m->occupied || !m->source_data || !seen) {
        h->release(seen); ofs_layout_close(m); return IFS_OFS_NO_MEMORY;
    }
    ofs_zero(m->used, m->map_bytes); ofs_zero(m->occupied, m->map_bytes);
    ofs_zero(m->source_data, m->map_bytes); ofs_zero(seen, m->map_bytes);
    for (uint32_t k = 0; k < f->reserved; ++k) set(m->used, k, 1);
    uint8_t *b = f->scratch[5];
    for (uint32_t page = 0; !r && page < f->bitmap_count; ++page) {
        uint32_t key;
        r = ofs_bitmap_key(f, page, &key);
        if (!r) r = ofs_read_block(f, key, b, 0);
        uint64_t first = (uint64_t)f->reserved + (uint64_t)page * f->bitmap_bits;
        for (uint32_t j = 0; !r && j < f->bitmap_bits && first + j < f->blocks; ++j) {
            int used = !((ofs_word(b, 1u + j / 32u) >> (j % 32u)) & 1u);
            set(m->used, (uint32_t)(first + j), used);
            m->used_blocks += (unsigned)used;
        }
        if (!r) r = stopped(m);
    }
    if (!r) r = push_directory(m, f->root);
    set(seen, f->root, 1);
    for (size_t at = 0; !r && at < m->directory_count; ++at) {
        OfsDirCursor cursor = {0}; OfsNode n;
        while (!(r = ofs_next(f, m->directories[at], &cursor, &n))) {
            if (n.object_type != IFS_OFS_FILE && n.object_type != IFS_OFS_DIRECTORY) continue;
            if (bit(seen, n.object)) continue;
            set(seen, n.object, 1);
            r = n.object_type == IFS_OFS_FILE ? push_file(m, n.object) : push_directory(m, n.object);
            if (r) break;
        }
        if (r == IFS_OFS_END) r = stopped(m);
    }
    h->release(seen);
    /* Original keys give a deterministic order independent of hash collisions. */
    if (!r && m->file_count) {
        if (m->file_count > SIZE_MAX / sizeof(uint32_t) || m->file_count > UINT32_MAX)
            r = IFS_OFS_NO_MEMORY;
        else {
            uint32_t *keys = h->allocate(m->file_count * sizeof(uint32_t));
            if (!keys) r = IFS_OFS_NO_MEMORY;
            else {
                for (size_t i = 0; i < m->file_count; ++i) keys[i] = m->files[i].key;
                ofs_sort_keys(keys, (uint32_t)m->file_count);
                for (size_t i = 0; i < m->file_count; ++i) m->files[i].key = keys[i];
                h->release(keys);
            }
        }
    }
    for (size_t i = 0; !r && i < m->file_count; ++i) {
        OfsLayoutFile *file = &m->files[i]; OfsNode node;
        r = ofs_node(f, file->key, &node, f->scratch[5]);
        if (r) break;
        file->bytes = node.bytes;
        file->count = node.bytes ? (node.bytes - 1u) / (f->block_bytes - 24u) + 1u : 0;
        if ((uint64_t)file->count * sizeof(uint32_t) > SIZE_MAX) { r = IFS_OFS_NO_MEMORY; break; }
        if (file->count) {
            file->old_data = h->allocate((size_t)file->count * sizeof(uint32_t));
            file->new_data = h->allocate((size_t)file->count * sizeof(uint32_t));
            if (!file->old_data || !file->new_data) { r = IFS_OFS_NO_MEMORY; break; }
        }
        OfsFileCursor c = {0}; int fragmented = 0;
        for (uint32_t j = 0; !r && j < file->count; ++j) {
            r = ofs_pointer(f, file->key, j, file->count, &c, &file->old_data[j]);
            if (!r) {
                set(m->source_data, file->old_data[j], 1);
                if (j && file->old_data[j] != file->old_data[j - 1u] + 1u) fragmented = 1;
            }
        }
        if (fragmented) { ++m->fragmented_files; m->fragmented_blocks += file->count; }
        if (!r) r = stopped(m);
    }
    for (uint32_t k = f->reserved; !r && k < f->blocks; ++k) {
        if (!bit(m->used, k) || bit(m->source_data, k) || system_key(f, k)) continue;
        void *p = m->metadata;
        r = reserve(m, &p, &m->metadata_capacity, m->metadata_count + 1u, sizeof(OfsKeyMove));
        if (!r) { m->metadata = p; m->metadata[m->metadata_count++] = (OfsKeyMove){k, 0}; }
    }
    if (!r) ofs_copy(m->source_used, m->used, m->map_bytes);
    if (r) ofs_layout_close(m);
    return r;
}
static uint32_t free_run(OfsLayout *m, uint32_t *at) {
    uint32_t n = m->fs->blocks;
    while (*at < n && bit(m->occupied, *at)) ++*at;
    uint32_t start = *at;
    while (*at < n && !bit(m->occupied, *at)) ++*at;
    return *at - start;
}
int ofs_layout_plan(OfsLayout *m, int growth) {
    OfsFs *f = m->fs;
    ofs_zero(m->occupied, m->map_bytes);
    for (uint32_t k = 0; k < f->reserved; ++k) set(m->occupied, k, 1);
    for (uint32_t i = 0; i < f->system_count; ++i) set(m->occupied, f->system_keys[i], 1);
    uint32_t next = f->reserved;
    for (size_t i = 0; i < m->metadata_count; ++i) {
        while (next < f->blocks && bit(m->occupied, next)) ++next;
        if (next == f->blocks) return IFS_OFS_NO_SPACE;
        m->metadata[i].new_key = next; set(m->occupied, next++, 1);
    }
    for (size_t i = 0; i < m->file_count; ++i) {
        OfsLayoutFile *file = &m->files[i];
        uint32_t gap = growth ? (uint32_t)(((uint64_t)file->count + 9u) / 10u) : 0;
        uint64_t need = (uint64_t)file->count + gap;
        uint32_t at = f->reserved, start = 0, run;
        if (!need) continue;
        /* Prefer one extent. Only a file larger than every available run is
         * split at fixed root/bitmap boundaries. A growth gap is real free
         * allocation immediately after the final data block, never metadata. */
        while (at < f->blocks) {
            run = free_run(m, &at);
            if (run >= need) { start = at - run; break; }
        }
        uint32_t done = 0;
        if (start) {
            for (uint32_t j = 0; j < file->count; ++j) file->new_data[j] = start + j;
            for (uint64_t j = 0; j < need; ++j) set(m->occupied, (uint32_t)(start + j), 1);
            done = file->count;
        } else {
            at = f->reserved;
            while (done < file->count && at < f->blocks) {
                run = free_run(m, &at); start = at - run;
                uint32_t remaining = file->count - done;
                uint32_t take = run < remaining ? run : remaining;
                if (take == remaining && (uint64_t)remaining + gap > run) --take;
                for (uint32_t j = 0; j < take; ++j) {
                    file->new_data[done++] = start + j; set(m->occupied, start + j, 1);
                }
                if (done == file->count)
                    for (uint32_t j = 0; j < gap; ++j) set(m->occupied, start + take + j, 1);
            }
        }
        if (done != file->count) return IFS_OFS_NO_SPACE;
        int r = stopped(m); if (r) return r;
    }
    /* Reserved Growth gaps constrain planning but are not allocated on disk. */
    ofs_zero(m->used, m->map_bytes);
    for (uint32_t k = 0; k < f->reserved; ++k) set(m->used, k, 1);
    for (uint32_t i = 0; i < f->system_count; ++i) set(m->used, f->system_keys[i], 1);
    for (size_t i = 0; i < m->metadata_count; ++i) set(m->used, m->metadata[i].new_key, 1);
    for (size_t i = 0; i < m->file_count; ++i)
        for (uint32_t j = 0; j < m->files[i].count; ++j) set(m->used, m->files[i].new_data[j], 1);
    m->planned = 1;
    return 0;
}
static int remap(OfsLayout *m, uint8_t *b, uint32_t offset) {
    uint32_t old = ofs_be32(b + offset), key = mapped(m, old);
    if (old && !key) return IFS_OFS_CORRUPT;
    ofs_put32(b + offset, key); return 0;
}
static int patch(OfsLayout *m, uint8_t *b, uint32_t old) {
    OfsFs *f = m->fs; uint32_t size = f->block_bytes, type = ofs_word(b, 0);
    int r = 0;
    if (old != f->root) ofs_setword(b, 1, mapped(m, old));
    if (type == 64u) r = remap(m, b, 8);
    else if (type == 33u) {
        r = remap(m, b, 8); if (!r) r = remap(m, b, 16);
        uint32_t at = 24, count = ofs_word(b, 3);
        for (uint32_t i = 0; !r && i < count; ++i) {
            if (size - at < 26u) return IFS_OFS_CORRUPT;
            uint32_t n = b[at + 23u];
            if (n > 30u || n + 25u > size - at) return IFS_OFS_CORRUPT;
            uint32_t c = b[at + 24u + n], bytes = (26u + n + c) & ~1u;
            if (c > 22u || bytes > size - at) return IFS_OFS_CORRUPT;
            r = remap(m, b, at); at += bytes;
        }
    } else if (type == 16u) {
        r = remap(m, b, size - 12u); if (!r) r = remap(m, b, size - 8u);
    } else if (type == 2u) {
        int32_t secondary = (int32_t)ofs_be32(b + size - 4u);
        if (secondary == IFS_OFS_ROOT || secondary == IFS_OFS_DIRECTORY)
            for (uint32_t j = 0; !r && j < f->hash_size; ++j) r = remap(m, b, 24u + 4u * j);
        if (old != f->root) {
            if (!r) r = remap(m, b, size - 16u);
            if (!r) r = remap(m, b, size - 12u);
            if (!r && secondary != IFS_OFS_SOFTLINK) r = remap(m, b, size - 40u);
            if (!r && (secondary == IFS_OFS_LINKFILE || secondary == IFS_OFS_LINKDIR))
                r = remap(m, b, size - 44u);
            if (!r && f->variant == 6u) r = remap(m, b, size - 72u);
        }
        if (!r && (secondary == IFS_OFS_FILE ||
            (f->variant == 4u && (secondary == IFS_OFS_ROOT || secondary == IFS_OFS_DIRECTORY))))
            r = remap(m, b, size - 8u);
    } else r = IFS_OFS_CORRUPT;
    if (!r) ofs_checksum(f, b, 5);
    return r;
}
static int write_block(OfsLayout *m, IfsDefragTarget *t, uint32_t key, const void *p) {
    uint64_t at = (uint64_t)key * m->fs->block_bytes;
    const IfsDefragHostV2 *h = m->io.host;
    if (at > t->bytes || m->fs->block_bytes > t->bytes - at) return IFS_OFS_INVALID;
    if (h->write_at(h->context, t, at, p, m->fs->block_bytes)) return IFS_OFS_IO_ERROR;
    return stopped(m);
}
static int read_metadata(OfsLayout *m, uint32_t key, uint8_t *b) {
    int r = ofs_read_block(m->fs, key, b, 0);
    if (!r && (ifs_ofs_block_checksum(b, m->fs->block_bytes) ||
               (key != m->fs->root && ofs_word(b, 1) != key))) r = IFS_OFS_CORRUPT;
    return r;
}
int ofs_layout_write(OfsLayout *m, IfsDefragTarget *target) {
    if (!m->planned || target->bytes != m->io.target->bytes) return IFS_OFS_INVALID;
    OfsFs *f = m->fs; const IfsDefragHostV2 *h = m->io.host; uint8_t *b = f->scratch[5];
    int r = 0;
    for (uint32_t k = 0; !r && k < f->reserved; ++k) {
        r = ofs_read_block(f, k, b, 0); if (!r) r = write_block(m, target, k, b);
    }
    for (uint32_t i = 0; !r && i < f->system_count; ++i) {
        uint32_t k = f->system_keys[i]; r = ofs_read_block(f, k, b, 0);
        if (!r && k == f->root) {
            r = patch(m, b, k); ofs_put32(b + f->block_bytes - 200u, 0); ofs_checksum(f, b, 5);
        }
        if (!r) r = write_block(m, target, k, b);
    }
    for (size_t i = 0; !r && i < m->metadata_count; ++i) {
        uint32_t k = m->metadata[i].old_key;
        r = read_metadata(m, k, b);
        if (r) break;
        if (ofs_word(b, 0) == 16u || (int32_t)ofs_be32(b + f->block_bytes - 4u) == IFS_OFS_FILE)
            continue;
        r = patch(m, b, k); if (!r) r = write_block(m, target, m->metadata[i].new_key, b);
    }
    for (size_t i = 0; !r && i < m->file_count; ++i) {
        OfsLayoutFile *file = &m->files[i];
        uint32_t list = file->key, done = 0;
        do {
            r = read_metadata(m, list, b); if (r) break;
            uint32_t next_list = ofs_be32(b + f->block_bytes - 8u);
            uint32_t count = file->count - done;
            if (count > f->hash_size) count = f->hash_size;
            r = patch(m, b, list); if (r) break;
            for (uint32_t j = 0; j < f->hash_size; ++j)
                ofs_put32(b + f->block_bytes - 204u - 4u * j,
                          j < count ? file->new_data[done + j] : 0);
            if (list == file->key) ofs_setword(b, 4, file->count ? file->new_data[0] : 0);
            ofs_checksum(f, b, 5); r = write_block(m, target, mapped(m, list), b);
            done += count; list = next_list;
        } while (!r && list);
        if (!r && done != file->count) r = IFS_OFS_CORRUPT;
        for (uint32_t j = 0; !r && j < file->count; ++j) {
            r = ofs_data(f, file->key, file->bytes, j, file->old_data[j], b);
            if (!r) {
                ofs_setword(b, 1, mapped(m, file->key));
                ofs_setword(b, 4, j + 1u < file->count ? file->new_data[j + 1u] : 0);
                ofs_checksum(f, b, 5); r = write_block(m, target, file->new_data[j], b);
            }
        }
    }
    for (uint32_t page = 0; !r && page < f->bitmap_count; ++page) {
        uint32_t key; r = ofs_bitmap_key(f, page, &key); if (r) break;
        ofs_zero(b, f->block_bytes);
        uint64_t first = (uint64_t)f->reserved + (uint64_t)page * f->bitmap_bits;
        for (uint32_t j = 0; j < f->bitmap_bits && first + j < f->blocks; ++j)
            if (!bit(m->used, (uint32_t)(first + j)))
                ofs_setword(b, 1u + j / 32u, ofs_word(b, 1u + j / 32u) | (UINT32_C(1) << (j % 32u)));
        ofs_checksum(f, b, 0); r = write_block(m, target, key, b);
    }
    if (!r && h->sync_target(h->context, target)) r = IFS_OFS_IO_ERROR;
    if (!r) r = ofs_metadata(f, f->root, b);
    if (!r) r = patch(m, b, f->root);
    if (!r) {
        ofs_put32(b + f->block_bytes - 200u, UINT32_MAX);
        if (f->variant == 6u) ofs_put32(b + f->block_bytes - 44u, (uint32_t)m->used_blocks);
        ofs_checksum(f, b, 5); r = write_block(m, target, f->root, b);
    }
    if (!r && h->sync_target(h->context, target)) r = IFS_OFS_IO_ERROR;
    return r;
}
int ofs_layout_verify(OfsLayout *m, int growth) {
    int r = ofs_layout_plan(m, growth);
    for (size_t i = 0; !r && i < m->metadata_count; ++i)
        if (m->metadata[i].old_key != m->metadata[i].new_key) r = IFS_OFS_CORRUPT;
    for (size_t i = 0; !r && i < m->file_count; ++i)
        for (uint32_t j = 0; !r && j < m->files[i].count; ++j)
            if (m->files[i].old_data[j] != m->files[i].new_data[j]) r = IFS_OFS_CORRUPT;
    return r;
}
static void hex(const uint8_t in[32], char out[65]) {
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) { out[2u*i] = digits[in[i] >> 4]; out[2u*i+1u] = digits[in[i] & 15u]; }
    out[64] = 0;
}
static int hash_map(OfsLayout *m, IfsDefragTarget *target, const uint8_t *map, char out[65]) {
    const IfsDefragHostV2 *h = m->io.host; void *digest = h->digest_create(h->context);
    if (!digest) return IFS_OFS_NO_MEMORY;
    uint8_t *b = m->fs->scratch[5], result[32], offset[8]; int r = 0;
    for (uint32_t k = 0; !r && k < m->fs->blocks; ++k) {
        if (!bit(map, k)) continue;
        uint64_t at = (uint64_t)k * m->fs->block_bytes;
        ofs_put32(offset, (uint32_t)(at >> 32)); ofs_put32(offset + 4, (uint32_t)at);
        if (h->read_at(h->context, target, at, b, m->fs->block_bytes) ||
            h->digest_update(h->context, digest, offset, sizeof(offset)) ||
            h->digest_update(h->context, digest, b, m->fs->block_bytes)) r = IFS_OFS_IO_ERROR;
        if (!r) r = stopped(m);
    }
    if (!r && h->digest_finish(h->context, digest, result)) r = IFS_OFS_IO_ERROR;
    h->digest_release(h->context, digest); if (!r) hex(result, out);
    return r;
}
int ofs_layout_hash(OfsLayout *m, IfsDefragTarget *target, char out[65]) {
    return hash_map(m, target, m->used, out);
}
int ofs_layout_source_hash(OfsLayout *m, IfsDefragTarget *target, char out[65]) {
    return hash_map(m, target, m->source_used, out);
}
int ofs_layout_token(const IfsDefragHostV2 *h, IfsDefragTarget *t, uint32_t bs,
                     uint32_t blocks, uint32_t reserved, uint32_t root, uint32_t variant,
                     char out[65]) {
    if (bs < 512u || bs > OFS_MAX_BLOCK || (bs & (bs-1u)) || root >= blocks ||
        reserved >= blocks || (uint64_t)blocks * bs > t->bytes) return IFS_OFS_CORRUPT;
    uint8_t *b = h->allocate(bs), result[32], fields[20];
    void *digest = h->digest_create(h->context);
    if (!b || !digest) { h->release(b); h->digest_release(h->context, digest); return IFS_OFS_NO_MEMORY; }
    ofs_put32(fields, bs); ofs_put32(fields+4, blocks); ofs_put32(fields+8, reserved);
    ofs_put32(fields+12, root); ofs_put32(fields+16, variant);
    int r = h->digest_update(h->context, digest, fields, sizeof(fields));
    for (uint32_t k = 0; !r && k < reserved; ++k) {
        r = h->read_at(h->context, t, (uint64_t)k * bs, b, bs);
        if (!r && !k && ofs_be32(b) != (UINT32_C(0x444f5300) | variant)) r = -1;
        if (!r) r = h->digest_update(h->context, digest, b, bs);
    }
    if (!r) r = h->read_at(h->context, t, (uint64_t)root * bs, b, bs);
    if (!r) r = h->digest_update(h->context, digest, b + bs - 80u, 32u);
    if (!r) r = h->digest_update(h->context, digest, b + bs - 92u, 12u);
    if (!r) r = h->digest_update(h->context, digest, b + bs - 40u, 24u);
    if (!r) r = h->digest_finish(h->context, digest, result);
    h->digest_release(h->context, digest); h->release(b);
    if (!r) hex(result, out);
    return r ? IFS_OFS_IO_ERROR : 0;
}
int ofs_layout_commit(OfsLayout *stage, IfsDefragTarget *target) {
    OfsFs *f = stage->fs; const IfsDefragHostV2 *h = stage->io.host;
    uint8_t *buffer = h->allocate(1024u * 1024u), *check = h->allocate(1024u * 1024u);
    if (!buffer || !check) { h->release(buffer); h->release(check); return IFS_OFS_NO_MEMORY; }
    int r = 0; uint64_t sequence = 0;
    h->print(h->context, 0, "OFS source commit: writing verified allocated blocks only.\n");
    /* Publish root last. A journal and complete verified stage already exist;
     * interruption may make the old graph unreadable, but never loses the
     * authoritative bytes needed by Recover. Boot blocks need no rewrite. */
    for (uint32_t k = f->reserved; !r && k < f->blocks;) {
        if (k == f->root || !bit(stage->used, k)) { ++k; continue; }
        uint32_t count = 1, max = (1024u * 1024u) / f->block_bytes;
        while ((uint64_t)k + count < f->blocks && k + count != f->root && count < max &&
               bit(stage->used, k + count)) ++count;
        uint32_t bytes = count * f->block_bytes; uint64_t at = (uint64_t)k * f->block_bytes;
        r = stopped(stage);
        if (!r && (h->read_at(h->context, stage->io.target, at, buffer, bytes) ||
                   h->write_at(h->context, target, at, buffer, bytes))) r = IFS_OFS_IO_ERROR;
        if (!r && stage->live)
            h->print(h->context, 0, "@@LIVE_RANGES {\"ranges\":[[%u,%u,1]],\"sequence\":%llu}\n",
                     k, k + count, (unsigned long long)++sequence);
        k += count;
    }
    if (!r && h->sync_target(h->context, target)) r = IFS_OFS_IO_ERROR;
    if (!r) {
        uint64_t at = (uint64_t)f->root * f->block_bytes;
        if (h->read_at(h->context, stage->io.target, at, buffer, f->block_bytes) ||
            h->write_at(h->context, target, at, buffer, f->block_bytes) ||
            h->sync_target(h->context, target)) r = IFS_OFS_IO_ERROR;
    }
    for (uint32_t k = 0; !r && k < f->blocks; ++k) {
        if (!bit(stage->used, k)) continue;
        uint64_t at = (uint64_t)k * f->block_bytes;
        if (h->read_at(h->context, stage->io.target, at, buffer, f->block_bytes) ||
            h->read_at(h->context, target, at, check, f->block_bytes) ||
            !ofs_equal(buffer, check, f->block_bytes)) r = IFS_OFS_IO_ERROR;
        if (!r) r = stopped(stage);
    }
    if (r == IFS_DEFRAG_STOPPED && h->sync_target(h->context, target)) r = IFS_OFS_IO_ERROR;
    h->release(buffer); h->release(check); return r;
}
static void ranges(OfsLayout *m, const uint8_t *map, int want) {
    const IfsDefragHostV2 *h = m->io.host; int comma = 0;
    h->print(h->context, 0, "[");
    for (uint32_t k = 0; k < m->fs->blocks;) {
        while (k < m->fs->blocks && bit(map, k) != want) ++k;
        if (k == m->fs->blocks) break;
        uint32_t first = k;
        while (k < m->fs->blocks && bit(map, k) == want) ++k;
        h->print(h->context, 0, "%s[%u,%u]", comma ? "," : "", first, k); comma = 1;
    }
    h->print(h->context, 0, "]");
}
int ofs_layout_analyse(OfsLayout *m) {
    const IfsDefragHostV2 *h = m->io.host; OfsFs *f = m->fs;
    h->print(h->context, 0, "{\"filesystem\":\"affs\",\"variant\":\"OFS\",\"dostype\":%u,"
        "\"block_size\":%u,\"total_blocks\":%u,\"regular_files\":%zu,\"directories\":%zu,"
        "\"fragmented_files\":%llu,\"fragmented_directories\":0,\"fragmented_blocks\":%llu,\"free_ranges\":",
        f->variant, f->block_bytes, f->blocks, m->file_count, m->directory_count,
        (unsigned long long)m->fragmented_files, (unsigned long long)m->fragmented_blocks);
    ranges(m, m->used, 0); h->print(h->context, 0, ",\"fragmented_ranges\":[");
    int comma = 0;
    for (size_t i = 0; i < m->file_count; ++i) {
        OfsLayoutFile *file = &m->files[i]; int fragmented = 0;
        for (uint32_t j = 1; j < file->count; ++j)
            if (file->old_data[j] != file->old_data[j-1u] + 1u) fragmented = 1;
        if (!fragmented) continue;
        for (uint32_t j = 0; j < file->count; ++j) {
            uint32_t k = file->old_data[j];
            h->print(h->context, 0, "%s[%u,%llu]", comma ? "," : "", k,
                     (unsigned long long)k + 1u); comma = 1;
        }
    }
    h->print(h->context, 0, "],\"directory_ranges\":[");
    for (size_t i = 0; i < m->directory_count; ++i)
        h->print(h->context, 0, "%s[%u,%llu]", i ? "," : "", m->directories[i],
                 (unsigned long long)m->directories[i] + 1u);
    h->print(h->context, 0, "]}\n"); return 0;
}
