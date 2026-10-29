/* SPDX-License-Identifier: GPL-3.0-or-later
 * Portable volume/allocation implementation. Bitmap geometry, polarity and
 * checksums reuse ofs_primitives and Linux storage.c. Formatting follows the
 * existing tools/ofs-image implementation, with streamed bitmap generation.
 * Normative contract: OFS_SPECIFICATION sections 3..7, 13 and 14.
 */
#include "ofs_engine.h"

uint32_t ofs_be32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
void ofs_put32(void *p, uint32_t n) {
    uint8_t *b = p;
    b[0] = (uint8_t)(n >> 24);
    b[1] = (uint8_t)(n >> 16);
    b[2] = (uint8_t)(n >> 8);
    b[3] = (uint8_t)n;
}
uint16_t ofs_be16(const void *p) {
    const uint8_t *b = p;
    return (uint16_t)((uint16_t)b[0] << 8 | b[1]);
}
void ofs_put16(void *p, uint16_t n) {
    uint8_t *b = p;
    b[0] = (uint8_t)(n >> 8);
    b[1] = (uint8_t)n;
}
uint32_t ofs_word(const uint8_t *b, uint32_t n) { return ofs_be32(b + 4u * n); }
void ofs_setword(uint8_t *b, uint32_t i, uint32_t n) { ofs_put32(b + 4u * i, n); }
void ofs_zero(void *p, size_t n) {
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}
void ofs_copy(void *p, const void *q, size_t n) {
    uint8_t *b = p;
    const uint8_t *s = q;
    while (n--)
        *b++ = *s++;
}
int ofs_equal(const void *p, const void *q, size_t n) {
    const uint8_t *a = p, *b = q;
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}
int ofs_valid_block(const OfsFs *f, uint32_t k) { return k >= f->reserved && k < f->blocks; }
void ofs_invalidate(OfsFs *f) { f->cache[0].valid = f->cache[1].valid = 0; }
int ofs_read_block(OfsFs *f, uint32_t k, uint8_t *out, int data) {
    uint64_t at = (uint64_t)k * f->block_bytes;
    unsigned slot = data ? 1u : 0u;
    if (!out || k >= f->blocks || at > f->io.volume_bytes ||
        f->block_bytes > f->io.volume_bytes - at)
        return IFS_OFS_CORRUPT;
    if (f->cache[slot].valid && at >= f->cache[slot].offset &&
        at - f->cache[slot].offset + f->block_bytes <= f->cache[slot].bytes) {
        ofs_copy(out, f->cache[slot].data + (size_t)(at - f->cache[slot].offset), f->block_bytes);
        return 0;
    }
    uint64_t base = at & ~(uint64_t)(OFS_CACHE_BYTES - 1u);
    uint64_t left = f->io.volume_bytes - base;
    uint32_t count = left > OFS_CACHE_BYTES ? OFS_CACHE_BYTES : (uint32_t)left;
    f->cache[slot].valid = 0;
    /* A bad unrequested sector in speculative read-ahead must not hide a
     * readable requested block. The exact read is the final authority. */
    if (f->io.read_at(f->io.context, base, f->cache[slot].data, count) == 0) {
        f->cache[slot].offset = base;
        f->cache[slot].bytes = count;
        f->cache[slot].valid = 1;
        ofs_copy(out, f->cache[slot].data + (size_t)(at - base), f->block_bytes);
        return 0;
    }
    return f->io.read_at(f->io.context, at, out, f->block_bytes) == 0 ? 0 : IFS_OFS_IO_ERROR;
}
int ofs_write_block(OfsFs *f, uint32_t k, const uint8_t *b) {
    if ((f->io.flags & IFS_OFS_READ_ONLY) || !f->io.write_at || k >= f->blocks)
        return IFS_OFS_ACCESS;
    ofs_invalidate(f);
    if (f->io.write_at(f->io.context, (uint64_t)k * f->block_bytes, b, f->block_bytes) != 0) {
        f->failed = 1;
        return IFS_OFS_IO_ERROR;
    }
    return 0;
}
void ofs_checksum(OfsFs *f, uint8_t *b, uint32_t w) {
    ofs_setword(b, w, ifs_ofs_checksum_word_value(b, f->block_bytes, w));
}
int ofs_metadata(OfsFs *f, uint32_t k, uint8_t *b) {
    if (!ofs_valid_block(f, k))
        return IFS_OFS_CORRUPT;
    int r = ofs_read_block(f, k, b, 0);
    if (r)
        return r;
    if (ofs_word(b, 0) != 2u || ifs_ofs_block_checksum(b, f->block_bytes))
        return IFS_OFS_CORRUPT;
    if (k != f->root && ofs_word(b, 1) != k)
        return IFS_OFS_CORRUPT;
    return 0;
}
IfsOfsDate ofs_get_date(const uint8_t *b) {
    return (IfsOfsDate){ofs_be32(b), ofs_be32(b + 4), ofs_be32(b + 8)};
}
void ofs_put_date(uint8_t *b, const IfsOfsDate *d) {
    ofs_put32(b, d->days);
    ofs_put32(b + 4, d->minutes);
    ofs_put32(b + 8, d->ticks);
}
int ofs_date_valid(const IfsOfsDate *d) { return d && d->minutes < 1440u && d->ticks < 3000u; }
int ofs_clock(OfsFs *f, IfsOfsDate *d) {
    if (!f->io.clock)
        return IFS_OFS_NOT_FOUND;
    if (f->io.clock(f->io.context, d) != 0 || !ofs_date_valid(d))
        return IFS_OFS_INVALID;
    return 0;
}
static int io_valid(const IfsOfsIoV3 *io) {
    return io && io->structure_bytes == sizeof(*io) && io->read_at && io->volume_bytes >= 8192u &&
           !(io->flags & ~(IFS_OFS_READ_ONLY | IFS_OFS_ALLOW_DIRTY)) &&
           ((io->flags & IFS_OFS_READ_ONLY) ||
            (io->write_at && io->flush && io->allocate && io->release));
}
static int block_size_valid(uint32_t b) {
    return b >= 512u && b <= OFS_MAX_BLOCK && !(b & (b - 1u));
}
static void sift(uint32_t *a, uint32_t n, uint32_t root) {
    for (;;) {
        uint64_t child = (uint64_t)root * 2u + 1u;
        if (child >= n)
            return;
        uint32_t c = (uint32_t)child;
        if (c + 1u < n && a[c] < a[c + 1u])
            ++c;
        if (a[root] >= a[c])
            return;
        uint32_t v = a[root];
        a[root] = a[c];
        a[c] = v;
        root = c;
    }
}
void ofs_sort_keys(uint32_t *a, uint32_t n) {
    for (uint32_t i = n / 2u; i > 0; --i)
        sift(a, n, i - 1u);
    for (uint32_t i = n; i > 1u; --i) {
        uint32_t v = a[0];
        a[0] = a[i - 1u];
        a[i - 1u] = v;
        sift(a, i - 1u, 0);
    }
}
static int system_key(OfsFs *f, uint32_t key) {
    if (!f->system_keys)
        return key == f->root;
    uint32_t lo = 0, hi = f->system_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (f->system_keys[mid] < key)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < f->system_count && f->system_keys[lo] == key;
}
void ofs_release(OfsFs *f) {
    while (f->orphans) {
        OfsOrphan *next = f->orphans->next;
        f->io.release(f->io.context, f->orphans);
        f->orphans = next;
    }
    if (f->bitmap_keys && f->io.release)
        f->io.release(f->io.context, f->bitmap_keys);
    if (f->cache_dirty && f->io.release)
        f->io.release(f->io.context, f->cache_dirty);
    f->cache_dirty = 0;
    f->cache_dirty_count = f->cache_dirty_capacity = 0;
    f->bitmap_keys = 0;
    f->system_keys = 0;
    f->system_count = 0;
    f->mounted = 0;
    ofs_invalidate(f);
}
static int index_failure(OfsFs *f, int status) {
    if (f->bitmap_keys)
        f->io.release(f->io.context, f->bitmap_keys);
    f->bitmap_keys = f->system_keys = 0;
    f->system_count = 0;
    return status;
}
int ofs_index_bitmaps(OfsFs *f, int allocation) {
    if (f->bitmap_keys)
        return 0;
    if (!f->io.allocate || !f->io.release)
        return IFS_OFS_NO_MEMORY;
    uint32_t per = f->block_bytes / 4u - 1u;
    uint32_t extensions = f->bitmap_count > 25u ? (f->bitmap_count - 25u + per - 1u) / per : 0u;
    uint32_t count = f->bitmap_count + extensions + 1u;
    f->bitmap_keys =
        f->io.allocate(f->io.context, (size_t)(f->bitmap_count + count) * sizeof(uint32_t));
    if (!f->bitmap_keys)
        return IFS_OFS_NO_MEMORY;
    f->system_keys = f->bitmap_keys + f->bitmap_count;
    f->system_count = count;
    uint8_t *b = f->scratch[6];
    int r = ofs_metadata(f, f->root, b);
    if (r)
        return index_failure(f, r);
    uint32_t seen = 0, extra = 0, next = ofs_be32(b + f->block_bytes - 96u);
    for (; seen < 25u && seen < f->bitmap_count; ++seen)
        f->bitmap_keys[seen] = ofs_be32(b + f->block_bytes - 196u + 4u * seen);
    while (seen < f->bitmap_count) {
        if (extra >= extensions || !ofs_valid_block(f, next))
            return index_failure(f, IFS_OFS_CORRUPT);
        f->system_keys[f->bitmap_count + extra++] = next;
        r = ofs_read_block(f, next, b, 0);
        if (r)
            return index_failure(f, r);
        for (uint32_t j = 0; j < per && seen < f->bitmap_count; ++j)
            f->bitmap_keys[seen++] = ofs_word(b, j);
        next = ofs_be32(b + f->block_bytes - 4u);
    }
    if (next)
        return index_failure(f, IFS_OFS_CORRUPT);
    for (uint32_t i = 0; i < f->bitmap_count; ++i)
        f->system_keys[i] = f->bitmap_keys[i];
    f->system_keys[count - 1u] = f->root;
    ofs_sort_keys(f->system_keys, count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!ofs_valid_block(f, f->system_keys[i]) ||
            (i && f->system_keys[i] == f->system_keys[i - 1u]))
            return index_failure(f, IFS_OFS_CORRUPT);
        if (allocation && !(f->io.flags & IFS_OFS_ALLOW_DIRTY)) {
            int free_bit;
            r = ofs_bitmap_free(f, f->system_keys[i], &free_bit);
            if (r)
                return index_failure(f, r);
            if (free_bit)
                return index_failure(f, IFS_OFS_CORRUPT);
        }
    }
    return 0;
}
int ofs_probe(OfsFs *f, const IfsOfsIoV3 *io, const IfsOfsMountV3 *opts) {
    uint8_t boot[12];
    uint32_t found = 0, chosen = 0, chosen_root = 0;
    if (!f || !io_valid(io) || (opts && opts->structure_bytes != sizeof(*opts)))
        return IFS_OFS_INVALID;
    if (io->read_at(io->context, 0, boot, sizeof(boot)) != 0)
        return IFS_OFS_IO_ERROR;
    if (boot[0] != 'D' || boot[1] != 'O' || boot[2] != 'S' ||
        (boot[3] != 0 && boot[3] != 2 && boot[3] != 4 && boot[3] != 6))
        return IFS_OFS_INVALID;
    uint32_t reserved = opts && opts->reserved_blocks ? opts->reserved_blocks : 2u;
    uint32_t requested = opts ? opts->block_bytes : 0;
    if ((requested && !block_size_valid(requested)) || reserved < 1)
        return IFS_OFS_INVALID;
    if (f->mounted || f->bitmap_keys)
        return IFS_OFS_INVALID;
    f->io = *io;
    f->variant = boot[3];
    f->reserved = reserved;
    f->mounted = 0;
    f->mutation = 0;
    f->failed = 0;
    for (uint32_t b = requested ? requested : 512u; b <= OFS_MAX_BLOCK; b <<= 1) {
        if (io->volume_bytes % b || io->volume_bytes / b > UINT32_MAX) {
            if (requested)
                break;
            continue;
        }
        uint32_t n = (uint32_t)(io->volume_bytes / b), root;
        if ((uint64_t)n <= (uint64_t)reserved + 2u || ifs_ofs_root_block(reserved, n, &root)) {
            if (requested)
                break;
            continue;
        }
        if (opts && opts->root_block)
            root = opts->root_block;
        /* A stored root key is useful for externally supplied hardfiles.
         * Geometry and the complete root structure still have to validate. */
        else if (ofs_be32(boot + 8))
            root = ofs_be32(boot + 8);
        f->block_bytes = b;
        f->blocks = n;
        f->root = root;
        f->hash_size = b / 4u - 56u;
        ofs_invalidate(f);
        uint8_t *r = f->scratch[6];
        if (ofs_metadata(f, root, r) == 0 && ofs_word(r, 1) == 0 && ofs_word(r, 2) == 0 &&
            ofs_word(r, 3) == f->hash_size && ofs_word(r, 4) == 0 && ofs_be32(r + b - 4u) == 1u &&
            r[b - 80u] <= 30u &&
            (!f->variant || f->variant != 6u || ofs_be32(r + b - 16u) == 0x444f5306u)) {
            ++found;
            chosen = b;
            chosen_root = root;
        }
        if (requested)
            break;
    }
    if (found != 1)
        return IFS_OFS_INVALID;
    f->block_bytes = chosen;
    f->blocks = (uint32_t)(io->volume_bytes / chosen);
    f->root = chosen_root;
    f->hash_size = chosen / 4u - 56u;
    ofs_invalidate(f);
    f->bitmap_ext = 0;
    f->bitmap_ext_index = 0;
    if (ifs_ofs_bitmap_geometry(chosen, reserved, f->blocks, &f->bitmap_bits, &f->bitmap_count))
        return IFS_OFS_INVALID;
    uint8_t *r = f->scratch[6];
    int status = ofs_metadata(f, f->root, r);
    if (status)
        return status;
    f->bitmap_valid = ofs_be32(r + chosen - 200u) == UINT32_MAX;
    if (!f->bitmap_valid && !(io->flags & IFS_OFS_ALLOW_DIRTY))
        return IFS_OFS_CORRUPT;
    uint32_t key;
    status = ofs_bitmap_key(f, 0, &key);
    if (status)
        return status;
    if (!(io->flags & IFS_OFS_READ_ONLY)) {
        status = ofs_index_bitmaps(f, 1);
        if (status) {
            ofs_release(f);
            return status;
        }
    } else if (!(io->flags & IFS_OFS_ALLOW_DIRTY)) {
        int free_bit;
        status = ofs_bitmap_free(f, f->root, &free_bit);
        if (status || free_bit)
            return status ? status : IFS_OFS_CORRUPT;
    }
    f->alloc_hint = reserved;
    f->mounted = 1;
    /* A valid flag does not make a contradictory allocation map safe. Verify
     * ownership before enabling any allocator on an ordinary writable mount.
     * Read-only Boot retains its bounded, heap-free streaming path. */
    if (!(io->flags & (IFS_OFS_READ_ONLY | IFS_OFS_ALLOW_DIRTY))) {
        IfsOfsCheckV3 check;
        status = ofs_verify(f, &check, 0);
        if (status) {
            ofs_release(f);
            return status;
        }
        f->used_blocks = (uint32_t)check.referenced_blocks;
    }
    return 0;
}
int ofs_bitmap_key(OfsFs *f, uint32_t index, uint32_t *key) {
    uint8_t *b = f->scratch[6];
    uint32_t k;
    if (index >= f->bitmap_count || !key)
        return IFS_OFS_CORRUPT;
    if (f->bitmap_keys) {
        *key = f->bitmap_keys[index];
        return ofs_valid_block(f, *key) ? 0 : IFS_OFS_CORRUPT;
    }
    int r = ofs_metadata(f, f->root, b);
    if (r)
        return r;
    if (index < 25u)
        k = ofs_be32(b + f->block_bytes - 196u + index * 4u);
    else {
        uint32_t per = f->block_bytes / 4u - 1u, want = (index - 25u) / per;
        k = ofs_be32(b + f->block_bytes - 96u);
        uint32_t current = 0;
        if (f->bitmap_ext && f->bitmap_ext_index <= want) {
            k = f->bitmap_ext;
            current = f->bitmap_ext_index;
        }
        for (;;) {
            if (!ofs_valid_block(f, k) || k == f->root)
                return IFS_OFS_CORRUPT;
            r = ofs_read_block(f, k, b, 0);
            if (r)
                return r;
            if (current == want)
                break;
            uint32_t next = ofs_be32(b + f->block_bytes - 4u);
            if (next == k)
                return IFS_OFS_CORRUPT;
            k = next;
            ++current;
        }
        f->bitmap_ext = k;
        f->bitmap_ext_index = want;
        k = ofs_word(b, (index - 25u) % per);
    }
    if (!ofs_valid_block(f, k) || k == f->root)
        return IFS_OFS_CORRUPT;
    *key = k;
    return 0;
}
int ofs_bitmap_free(OfsFs *f, uint32_t block, int *free_bit) {
    if (!ofs_valid_block(f, block) || !free_bit)
        return IFS_OFS_CORRUPT;
    uint32_t relative = block - f->reserved, key;
    int r = ofs_bitmap_key(f, relative / f->bitmap_bits, &key);
    if (r)
        return r;
    uint8_t *b = f->scratch[7];
    r = ofs_read_block(f, key, b, 0);
    if (r)
        return r;
    if (ifs_ofs_block_checksum(b, f->block_bytes))
        return IFS_OFS_CORRUPT;
    uint32_t bit = relative % f->bitmap_bits;
    *free_bit = (ofs_word(b, 1u + bit / 32u) & (UINT32_C(1) << (bit % 32u))) != 0;
    return 0;
}
int ofs_bitmap_set(OfsFs *f, uint32_t block, int free_bit) {
    if (!ofs_valid_block(f, block) || system_key(f, block))
        return IFS_OFS_CORRUPT;
    uint32_t relative = block - f->reserved, key;
    int r = ofs_bitmap_key(f, relative / f->bitmap_bits, &key);
    if (r)
        return r;
    if (block == key)
        return IFS_OFS_CORRUPT;
    uint8_t *b = f->scratch[7];
    r = ofs_read_block(f, key, b, 0);
    if (r)
        return r;
    if (ifs_ofs_block_checksum(b, f->block_bytes))
        return IFS_OFS_CORRUPT;
    uint32_t bit = relative % f->bitmap_bits, at = 1u + bit / 32u,
             mask = UINT32_C(1) << (bit % 32u), value = ofs_word(b, at);
    if (((value & mask) != 0) == (free_bit != 0))
        return IFS_OFS_CORRUPT;
    ofs_setword(b, at, free_bit ? value | mask : value & ~mask);
    ofs_checksum(f, b, 0);
    r = ofs_write_block(f, key, b);
    if (!r && f->variant == 6u) {
        if (free_bit)
            --f->used_blocks;
        else
            ++f->used_blocks;
    }
    return r;
}
int ofs_allocate(OfsFs *f, uint32_t *key) {
    uint32_t start = f->alloc_hint;
    if (start < f->reserved || start >= f->blocks)
        start = f->reserved;
    uint32_t current = start;
    uint64_t remaining = (uint64_t)f->blocks - f->reserved;
    while (remaining) {
        uint32_t rel = current - f->reserved, index = rel / f->bitmap_bits, bm;
        int r = ofs_bitmap_key(f, index, &bm);
        if (r)
            return r;
        uint8_t *b = f->scratch[7];
        r = ofs_read_block(f, bm, b, 0);
        if (r)
            return r;
        if (ifs_ofs_block_checksum(b, f->block_bytes))
            return IFS_OFS_CORRUPT;
        uint32_t bit = rel % f->bitmap_bits, word_index = 1u + bit / 32u;
        uint32_t step = 32u - bit % 32u;
        if (step > f->blocks - current)
            step = f->blocks - current;
        if (step > remaining)
            step = (uint32_t)remaining;
        uint32_t mask = step == 32u ? UINT32_MAX : ((UINT32_C(1) << step) - 1u) << (bit % 32u);
        uint32_t available = ofs_word(b, word_index) & mask;
        while (available) {
            uint32_t offset = (uint32_t)__builtin_ctz(available);
            uint64_t candidate = (uint64_t)f->reserved + (uint64_t)index * f->bitmap_bits +
                                 (uint64_t)(word_index - 1u) * 32u + offset;
            if (candidate >= f->blocks)
                break;
            if (system_key(f, (uint32_t)candidate) || candidate == bm)
                return IFS_OFS_CORRUPT;
            ofs_setword(b, word_index, ofs_word(b, word_index) & ~(UINT32_C(1) << offset));
            ofs_checksum(f, b, 0);
            r = ofs_write_block(f, bm, b);
            if (r)
                return r;
            if (f->variant == 6u)
                ++f->used_blocks;
            *key = (uint32_t)candidate;
            f->alloc_hint = *key + 1u < f->blocks ? *key + 1u : f->reserved;
            return 0;
        }
        /* Advance a whole bitmap word, including its out-of-volume tail. */
        uint64_t next = (uint64_t)current + step;
        current = next >= f->blocks ? f->reserved : (uint32_t)next;
        remaining -= step;
    }
    return IFS_OFS_NO_SPACE;
}
int ofs_begin(OfsFs *f) {
    if (!f->mounted || f->mutation || !f->bitmap_valid ||
        (f->io.flags & (IFS_OFS_READ_ONLY | IFS_OFS_ALLOW_DIRTY)))
        return IFS_OFS_ACCESS;
    uint8_t *r = f->scratch[6];
    int status = ofs_metadata(f, f->root, r);
    if (status)
        return status;
    ofs_put32(r + f->block_bytes - 200u, 0);
    ofs_checksum(f, r, 5);
    f->bitmap_valid = 0;
    f->mutation = 1;
    f->preserve_volume_date = 0;
    f->cache_dirty_count = 0;
    status = ofs_write_block(f, f->root, r);
    if (!status && f->io.flush(f->io.context))
        status = IFS_OFS_IO_ERROR;
    if (status)
        f->failed = 1;
    return status;
}
int ofs_touch_root(OfsFs *f, int directory) {
    IfsOfsDate now;
    int r = ofs_clock(f, &now);
    if (r == IFS_OFS_NOT_FOUND)
        return 0;
    if (r)
        return r;
    uint8_t *b = f->scratch[6];
    r = ofs_metadata(f, f->root, b);
    if (r)
        return r;
    ofs_put_date(b + f->block_bytes - 40u, &now);
    if (directory)
        ofs_put_date(b + f->block_bytes - 92u, &now);
    ofs_checksum(f, b, 5);
    return ofs_write_block(f, f->root, b);
}
int ofs_end(OfsFs *f, int status) {
    if (!f->mutation)
        return status;
    if (status || f->failed) {
        f->mutation = 0;
        return status ? status : IFS_OFS_IO_ERROR;
    }
    status = ofs_cache_commit(f);
    if (!status && !f->preserve_volume_date)
        status = ofs_touch_root(f, 0);
    if (!status && f->io.flush(f->io.context))
        status = IFS_OFS_IO_ERROR;
    if (!status) {
        uint8_t *r = f->scratch[6];
        status = ofs_metadata(f, f->root, r);
        if (!status) {
            if (f->variant == 6u)
                ofs_put32(r + f->block_bytes - 44u, f->used_blocks);
            ofs_put32(r + f->block_bytes - 200u, f->orphans ? 0 : UINT32_MAX);
            ofs_checksum(f, r, 5);
            status = ofs_write_block(f, f->root, r);
        }
        if (!status && f->io.flush(f->io.context))
            status = IFS_OFS_IO_ERROR;
    }
    if (!status)
        f->bitmap_valid = 1;
    else
        f->failed = 1;
    f->mutation = 0;
    return status;
}
int ofs_space(OfsFs *f, IfsOfsSpaceV3 *out) {
    if (!f->mounted || !out)
        return IFS_OFS_INVALID;
    uint64_t free_count = 0;
    for (uint32_t i = 0; i < f->bitmap_count; ++i) {
        uint32_t key;
        int r = ofs_bitmap_key(f, i, &key);
        if (r)
            return r;
        uint8_t *b = f->scratch[7];
        r = ofs_read_block(f, key, b, 0);
        if (r)
            return r;
        if (ifs_ofs_block_checksum(b, f->block_bytes))
            return IFS_OFS_CORRUPT;
        uint64_t base = (uint64_t)i * f->bitmap_bits,
                 remaining = (uint64_t)f->blocks - f->reserved - base;
        uint32_t valid = remaining < f->bitmap_bits ? (uint32_t)remaining : f->bitmap_bits;
        for (uint32_t j = 0; j < valid; j += 32u) {
            uint32_t n = valid - j < 32u ? valid - j : 32u;
            uint32_t mask = n == 32u ? UINT32_MAX : (UINT32_C(1) << n) - 1u;
            uint32_t v = ofs_word(b, 1u + j / 32u) & mask;
            v -= (v >> 1) & UINT32_C(0x55555555);
            v = (v & UINT32_C(0x33333333)) + ((v >> 2) & UINT32_C(0x33333333));
            v = (v + (v >> 4)) & UINT32_C(0x0f0f0f0f);
            free_count += (v * UINT32_C(0x01010101)) >> 24;
        }
    }
    *out = (IfsOfsSpaceV3){sizeof(*out),
                           f->block_bytes,
                           f->blocks,
                           f->root,
                           f->variant,
                           (uint64_t)(f->blocks - f->reserved) * f->block_bytes,
                           free_count * f->block_bytes,
                           f->bitmap_valid};
    return 0;
}
/* Metadata keys occupy a streamed prefix, skipping the midpoint root. */
static uint32_t format_key(uint32_t reserved, uint32_t root, uint32_t index) {
    uint32_t k = reserved + index;
    return k >= root ? k + 1u : k;
}
int ofs_format(OfsFs *f, const IfsOfsIoV3 *io, const IfsOfsFormatV3 *opts) {
    uint8_t label[31];
    uint32_t length;
    if (!f || !io_valid(io) || io->flags || !opts || opts->structure_bytes != sizeof(*opts) ||
        !block_size_valid(opts->block_bytes) ||
        (opts->variant != 0 && opts->variant != 2 && opts->variant != 4 && opts->variant != 6) ||
        ofs_latin1(opts->label, label, 30, &length, 0) ||
        (length && ofs_name_valid(label, length, 30)))
        return IFS_OFS_INVALID;
    uint32_t b = opts->block_bytes, reserved = opts->reserved_blocks ? opts->reserved_blocks : 2u;
    if (reserved < 1 || io->volume_bytes % b || io->volume_bytes / b > UINT32_MAX ||
        io->volume_bytes / b < 16u)
        return IFS_OFS_INVALID;
    f->io = *io;
    f->block_bytes = b;
    f->blocks = (uint32_t)(io->volume_bytes / b);
    f->reserved = reserved;
    f->variant = opts->variant;
    if (ifs_ofs_root_block(reserved, f->blocks, &f->root) ||
        ifs_ofs_bitmap_geometry(b, reserved, f->blocks, &f->bitmap_bits, &f->bitmap_count))
        return IFS_OFS_INVALID;
    uint32_t per = b / 4u - 1u,
             extra = f->bitmap_count > 25u ? (f->bitmap_count - 25u + per - 1u) / per : 0u;
    uint64_t meta = (uint64_t)f->bitmap_count + extra + (opts->variant == 4u);
    if (meta + 1u >= f->blocks - reserved)
        return IFS_OFS_NO_SPACE;
    IfsOfsDate now = {0};
    int r = ofs_clock(f, &now);
    if (r != 0 && r != IFS_OFS_NOT_FOUND)
        return r;
    f->failed = 0;
    f->mounted = 0;
    f->mutation = 0;
    ofs_invalidate(f);
    uint8_t *data = f->scratch[0];
    /* Invalidate any old root and flush before replacing the boot area. */
    ofs_zero(data, b);
    ofs_setword(data, 0, 2);
    ofs_setword(data, 3, b / 4u - 56u);
    ofs_put32(data + b - 4u, 1);
    ofs_checksum(f, data, 5);
    r = ofs_write_block(f, f->root, data);
    if (r)
        return r;
    if (io->flush(io->context))
        return IFS_OFS_IO_ERROR;
    ofs_zero(data, b);
    data[0] = 'D';
    data[1] = 'O';
    data[2] = 'S';
    data[3] = (uint8_t)opts->variant;
    ofs_put32(data + 8, f->root);
    /* All remaining boot-area words are zero. One's-complement end-around
     * carry is deliberately different from a metadata negative-sum checksum.
     * This is a nonbootable filesystem, not invented Amiga boot code. */
    uint64_t sum = (uint64_t)ofs_be32(data) + f->root;
    sum = (sum & UINT32_MAX) + (sum >> 32);
    ofs_put32(data + 4, ~(uint32_t)sum);
    r = ofs_write_block(f, 0, data);
    if (r)
        return r;
    ofs_zero(data, b);
    for (uint32_t k = 1; k < reserved; ++k) {
        r = ofs_write_block(f, k, data);
        if (r)
            return r;
    }
    uint32_t end_key = format_key(reserved, f->root, (uint32_t)meta - 1u);
    for (uint32_t i = 0; i < f->bitmap_count; ++i) {
        ofs_zero(data, b);
        for (uint32_t w = 0; w < per; ++w) {
            uint64_t first = (uint64_t)reserved + (uint64_t)i * f->bitmap_bits + (uint64_t)w * 32u;
            uint32_t bits = 0;
            for (uint32_t j = 0; j < 32u && first + j < f->blocks; ++j) {
                uint32_t k = (uint32_t)(first + j);
                if (k != f->root && (k < reserved || k > end_key))
                    bits |= UINT32_C(1) << j;
            }
            ofs_setword(data, w + 1u, bits);
        }
        ofs_checksum(f, data, 0);
        r = ofs_write_block(f, format_key(reserved, f->root, i), data);
        if (r)
            return r;
    }
    for (uint32_t i = 0; i < extra; ++i) {
        ofs_zero(data, b);
        uint32_t first = 25u + i * per;
        for (uint32_t j = 0; j < per && first + j < f->bitmap_count; ++j)
            ofs_setword(data, j, format_key(reserved, f->root, first + j));
        if (i + 1u < extra)
            ofs_put32(data + b - 4u, format_key(reserved, f->root, f->bitmap_count + i + 1u));
        r = ofs_write_block(f, format_key(reserved, f->root, f->bitmap_count + i), data);
        if (r)
            return r;
    }
    ofs_zero(data, b);
    ofs_setword(data, 0, 2);
    ofs_setword(data, 3, b / 4u - 56u);
    for (uint32_t i = 0; i < 25u && i < f->bitmap_count; ++i)
        ofs_put32(data + b - 196u + 4u * i, format_key(reserved, f->root, i));
    if (extra)
        ofs_put32(data + b - 96u, format_key(reserved, f->root, f->bitmap_count));
    if (opts->variant == 4u) {
        uint32_t cache_key = format_key(reserved, f->root, f->bitmap_count + extra);
        uint8_t *cache = f->scratch[1];
        ofs_zero(cache, b);
        ofs_setword(cache, 0, 33);
        ofs_setword(cache, 1, cache_key);
        ofs_setword(cache, 2, f->root);
        ofs_checksum(f, cache, 5);
        r = ofs_write_block(f, cache_key, cache);
        if (r)
            return r;
        ofs_put32(data + b - 8u, cache_key);
    }
    if (opts->variant == 6u) {
        ofs_put32(data + b - 44u, (uint32_t)meta + 1u);
        ofs_put32(data + b - 16u, 0x444f5306u);
    }
    data[b - 80u] = (uint8_t)length;
    ofs_copy(data + b - 79u, label, length);
    ofs_put_date(data + b - 92u, &now);
    ofs_put_date(data + b - 40u, &now);
    ofs_put_date(data + b - 28u, &now);
    ofs_put32(data + b - 4u, 1);
    ofs_checksum(f, data, 5);
    r = ofs_write_block(f, f->root, data);
    if (r)
        return r;
    if (io->flush(io->context))
        return IFS_OFS_IO_ERROR;
    ofs_put32(data + b - 200u, UINT32_MAX);
    ofs_checksum(f, data, 5);
    r = ofs_write_block(f, f->root, data);
    if (r)
        return r;
    return io->flush(io->context) == 0 ? 0 : IFS_OFS_IO_ERROR;
}
