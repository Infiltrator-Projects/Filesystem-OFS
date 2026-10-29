/* SPDX-License-Identifier: GPL-3.0-or-later
 * DOS4 cache records are derived from the authoritative directory hashes.
 * Specification: OFS_SPECIFICATION section 12 and Olaf Barthel's DCFS layout.
 * Read-only Boot deliberately follows the hashes. Writable mounts verify all
 * caches; successful mutations rebuild every affected directory before the
 * bitmap can become valid. Host allocation/I/O failures leave it invalid.
 */
#include "ofs_engine.h"

typedef struct {
    uint32_t *keys;
    size_t count, capacity;
} Keys;
static int add(OfsFs *f, Keys *v, uint32_t key) {
    if (v->count == v->capacity) {
        size_t cap = v->capacity ? v->capacity * 2u : 16u;
        if (cap < v->capacity || cap > SIZE_MAX / sizeof(uint32_t))
            return IFS_OFS_NO_MEMORY;
        uint32_t *p = f->io.allocate(f->io.context, cap * sizeof(uint32_t));
        if (!p)
            return IFS_OFS_NO_MEMORY;
        if (v->keys) {
            ofs_copy(p, v->keys, v->count * sizeof(uint32_t));
            f->io.release(f->io.context, v->keys);
        }
        v->keys = p;
        v->capacity = cap;
    }
    v->keys[v->count++] = key;
    return 0;
}
static void release(OfsFs *f, Keys *v) {
    if (v->keys)
        f->io.release(f->io.context, v->keys);
}
int ofs_cache_queue(OfsFs *f, uint32_t key) {
    if (f->variant != 4u)
        return 0;
    if (!ofs_valid_block(f, key))
        return IFS_OFS_CORRUPT;
    for (size_t i = 0; i < f->cache_dirty_count; ++i)
        if (f->cache_dirty[i] == key)
            return 0;
    Keys v = {f->cache_dirty, f->cache_dirty_count, f->cache_dirty_capacity};
    int r = add(f, &v, key);
    f->cache_dirty = v.keys;
    f->cache_dirty_count = v.count;
    f->cache_dirty_capacity = v.capacity;
    return r;
}
int ofs_cache_object(OfsFs *f, uint32_t key) {
    if (f->variant != 4u || ofs_is_orphan(f, key))
        return 0;
    OfsNode object;
    int r = ofs_node(f, key, &object, f->scratch[5]);
    if (r)
        return r;
    if (object.object != key)
        return ofs_cache_object(f, object.object);
    if (key == f->root)
        return 0;
    r = ofs_cache_queue(f, object.parent);
    uint32_t next = object.link, seen = 0;
    while (!r && next) {
        if (++seen > f->blocks - f->reserved)
            return IFS_OFS_CORRUPT;
        OfsNode n;
        r = ofs_node(f, next, &n, f->scratch[5]);
        if (r)
            break;
        if (n.original != key || (n.type != IFS_OFS_LINKFILE && n.type != IFS_OFS_LINKDIR))
            return IFS_OFS_CORRUPT;
        r = ofs_cache_queue(f, n.parent);
        next = n.link;
    }
    return r;
}
static int cache_read(OfsFs *f, uint32_t key, uint32_t dir, uint8_t *b) {
    if (!ofs_valid_block(f, key) || key == dir)
        return IFS_OFS_CORRUPT;
    int r = ofs_read_block(f, key, b, 0);
    if (r)
        return r;
    return ofs_word(b, 0) == 33u && ofs_word(b, 1) == key && ofs_word(b, 2) == dir &&
                   !ifs_ofs_block_checksum(b, f->block_bytes)
               ? 0
               : IFS_OFS_CORRUPT;
}
static int chain(OfsFs *f, uint32_t dir, Keys *keys) {
    uint8_t *b = f->scratch[3];
    int r = ofs_directory(f, dir, b);
    if (r)
        return r;
    uint32_t key = ofs_be32(b + f->block_bytes - 8u), fast = key;
    if (!key)
        return IFS_OFS_CORRUPT;
    while (key) {
        if (keys->count >= f->blocks - f->reserved)
            return IFS_OFS_CORRUPT;
        r = cache_read(f, key, dir, b);
        if (r)
            return r;
        uint32_t next = ofs_word(b, 4);
        r = add(f, keys, key);
        if (r)
            return r;
        for (unsigned i = 0; i < 2 && fast; ++i) {
            r = cache_read(f, fast, dir, b);
            if (r)
                return r;
            fast = ofs_word(b, 4);
        }
        key = next;
        if (key && key == fast)
            return IFS_OFS_CORRUPT;
    }
    return 0;
}
static uint32_t record_size(const OfsNode *n) {
    uint32_t c = n->comment_bytes < 22u ? n->comment_bytes : 22u;
    return (26u + n->name_bytes + c) & ~1u;
}
static void put_record(uint8_t *b, const OfsNode *n) {
    uint32_t c = n->comment_bytes < 22u ? n->comment_bytes : 22u;
    ofs_zero(b, record_size(n));
    ofs_put32(b, n->key);
    ofs_put32(b + 4, n->type == IFS_OFS_FILE ? n->bytes : 0);
    ofs_put32(b + 8, n->protection);
    ofs_put16(b + 12, n->uid);
    ofs_put16(b + 14, n->gid);
    ofs_put16(b + 16, (uint16_t)n->changed.days);
    ofs_put16(b + 18, (uint16_t)n->changed.minutes);
    ofs_put16(b + 20, (uint16_t)n->changed.ticks);
    b[22] = (uint8_t)n->type;
    b[23] = (uint8_t)n->name_bytes;
    ofs_copy(b + 24, n->name, n->name_bytes);
    b[24u + n->name_bytes] = (uint8_t)c;
    ofs_copy(b + 25u + n->name_bytes, n->comment, c);
}
static void new_block(OfsFs *f, uint8_t *b, uint32_t key, uint32_t dir) {
    ofs_zero(b, f->block_bytes);
    ofs_setword(b, 0, 33);
    ofs_setword(b, 1, key);
    ofs_setword(b, 2, dir);
}
int ofs_cache_init(OfsFs *f, uint32_t dir, uint32_t *key) {
    if (f->variant != 4u) {
        *key = 0;
        return 0;
    }
    int r = ofs_allocate(f, key);
    if (r)
        return r;
    uint8_t *b = f->scratch[3];
    new_block(f, b, *key, dir);
    ofs_checksum(f, b, 5);
    return ofs_write_block(f, *key, b);
}
int ofs_cache_drop(OfsFs *f, uint32_t dir) {
    if (f->variant != 4u)
        return 0;
    Keys v = {0};
    int r = chain(f, dir, &v);
    for (size_t i = 0; !r && i < v.count; ++i)
        r = ofs_bitmap_set(f, v.keys[i], 1);
    release(f, &v);
    return r;
}
static int rebuild(OfsFs *f, uint32_t dir) {
    Keys v = {0};
    int r = chain(f, dir, &v);
    if (r) {
        release(f, &v);
        return r;
    }
    uint8_t *b = f->scratch[4];
    size_t at_key = 0;
    uint32_t at = 24, count = 0;
    new_block(f, b, v.keys[0], dir);
    OfsDirCursor cursor = {0};
    OfsNode n;
    while (!(r = ofs_next(f, dir, &cursor, &n))) {
        uint32_t size = record_size(&n);
        if (size > f->block_bytes - at) {
            if (at_key + 1u == v.count) {
                uint32_t next;
                r = ofs_allocate(f, &next);
                if (r)
                    break;
                r = add(f, &v, next);
                if (r)
                    break;
            }
            ofs_setword(b, 3, count);
            ofs_setword(b, 4, v.keys[at_key + 1u]);
            ofs_checksum(f, b, 5);
            r = ofs_write_block(f, v.keys[at_key], b);
            if (r)
                break;
            ++at_key;
            at = 24;
            count = 0;
            new_block(f, b, v.keys[at_key], dir);
        }
        put_record(b + at, &n);
        at += size;
        ++count;
    }
    if (r == IFS_OFS_END) {
        ofs_setword(b, 3, count);
        ofs_checksum(f, b, 5);
        r = ofs_write_block(f, v.keys[at_key], b);
        for (size_t i = at_key + 1u; !r && i < v.count; ++i)
            r = ofs_bitmap_set(f, v.keys[i], 1);
    }
    release(f, &v);
    return r;
}
int ofs_cache_commit(OfsFs *f) {
    for (size_t i = 0; i < f->cache_dirty_count; ++i) {
        int r = rebuild(f, f->cache_dirty[i]);
        if (r)
            return r;
    }
    return 0;
}
int ofs_cache_check(OfsFs *f, uint32_t dir, int (*reference)(void *, uint32_t), void *context) {
    if (f->variant != 4u)
        return 0;
    Keys blocks = {0}, entries = {0};
    int r = chain(f, dir, &blocks);
    uint8_t *b = f->scratch[4];
    for (size_t i = 0; !r && i < blocks.count; ++i) {
        if (reference) {
            r = reference(context, blocks.keys[i]);
            if (r)
                break;
        }
        r = cache_read(f, blocks.keys[i], dir, b);
        if (r)
            break;
        uint32_t count = ofs_word(b, 3), at = 24;
        if (count > (f->block_bytes - 24u) / 26u) {
            r = IFS_OFS_CORRUPT;
            break;
        }
        for (uint32_t j = 0; !r && j < count; ++j) {
            if (f->block_bytes - at < 26u) {
                r = IFS_OFS_CORRUPT;
                break;
            }
            uint8_t *p = b + at;
            uint32_t n = p[23];
            if (!n || n > 30u || n + 25u > f->block_bytes - at) {
                r = IFS_OFS_CORRUPT;
                break;
            }
            uint32_t c = p[24u + n], size = (26u + n + c) & ~1u;
            if (c > 22u || size > f->block_bytes - at) {
                r = IFS_OFS_CORRUPT;
                break;
            }
            OfsNode node;
            r = ofs_lookup(f, dir, p + 24, n, &node);
            if (r) {
                r = IFS_OFS_CORRUPT;
                break;
            }
            uint8_t expected[80];
            put_record(expected, &node);
            if (size != record_size(&node) || !ofs_equal(expected, p, 25u + n + c)) {
                r = IFS_OFS_CORRUPT;
                break;
            }
            r = add(f, &entries, node.key);
            at += size;
        }
    }
    if (!r) {
        if (entries.count > UINT32_MAX)
            r = IFS_OFS_CORRUPT;
        else
            ofs_sort_keys(entries.keys, (uint32_t)entries.count);
        for (size_t i = 1; !r && i < entries.count; ++i)
            if (entries.keys[i] == entries.keys[i - 1u])
                r = IFS_OFS_CORRUPT;
        OfsDirCursor cursor = {0};
        OfsNode node;
        size_t count = 0;
        while (!r) {
            int next = ofs_next(f, dir, &cursor, &node);
            if (next == IFS_OFS_END)
                break;
            if (next) {
                r = next;
                break;
            }
            ++count;
        }
        if (!r && count != entries.count)
            r = IFS_OFS_CORRUPT;
    }
    release(f, &blocks);
    release(f, &entries);
    return r;
}
