/* SPDX-License-Identifier: GPL-3.0-or-later
 * Whole-volume ownership/bitmap verification and explicit bitmap rebuild.
 * Hash entries, original objects and hard-link membership have independent
 * visited maps: shared file data must not look like duplicate allocation.
 * No directory recursion, platform I/O, or implicit repair occurs.
 * OFS_SPECIFICATION sections 7..16; existing ofs-image checker provenance.
 */
#include "ofs_engine.h"
typedef struct {
    uint32_t key;
    OfsDirCursor cursor;
} Frame;
typedef struct {
    OfsFs *fs;
    IfsOfsCheckV3 *report;
    uint8_t *memory, *referenced, *names, *objects, *links;
    size_t map_bytes;
    uint32_t *object_keys, *alias_keys;
    size_t object_count, object_capacity, alias_count, alias_capacity;
    Frame *frames;
    size_t depth, frame_capacity;
} Check;
static int bit(const uint8_t *p, uint32_t key) { return (p[key / 8u] >> (key % 8u)) & 1u; }
static int mark(uint8_t *p, uint32_t key) {
    int old = bit(p, key);
    p[key / 8u] |= (uint8_t)(1u << (key % 8u));
    return old;
}
static int bad(Check *c, uint32_t key, int status) {
    c->report->errors = 1;
    if (!c->report->first_bad_block)
        c->report->first_bad_block = key;
    return status;
}
static int reference(Check *c, uint32_t key) {
    if (!ofs_valid_block(c->fs, key) || mark(c->referenced, key))
        return bad(c, key, IFS_OFS_CORRUPT);
    ++c->report->referenced_blocks;
    return 0;
}
static int reserve(Check *c, void **old, size_t *capacity, size_t count, size_t bytes) {
    if (count <= *capacity)
        return 0;
    size_t next = *capacity ? *capacity : 32u;
    while (next < count) {
        if (next > SIZE_MAX / 2u)
            return IFS_OFS_NO_MEMORY;
        next *= 2u;
    }
    if (next > SIZE_MAX / bytes)
        return IFS_OFS_NO_MEMORY;
    void *p = c->fs->io.allocate(c->fs->io.context, next * bytes);
    if (!p)
        return IFS_OFS_NO_MEMORY;
    if (*old) {
        ofs_copy(p, *old, *capacity * bytes);
        c->fs->io.release(c->fs->io.context, *old);
    }
    *old = p;
    *capacity = next;
    return 0;
}
static int push_key(Check *c, uint32_t key, int alias) {
    uint32_t **p = alias ? &c->alias_keys : &c->object_keys;
    size_t *n = alias ? &c->alias_count : &c->object_count,
           *capacity = alias ? &c->alias_capacity : &c->object_capacity;
    void *memory = *p;
    int r = reserve(c, &memory, capacity, *n + 1u, sizeof(uint32_t));
    if (r)
        return r;
    *p = memory;
    (*p)[(*n)++] = key;
    return 0;
}
static int comment_reference(Check *c, uint32_t key) {
    OfsFs *f = c->fs;
    if (f->variant != 6u || key == f->root)
        return 0;
    uint8_t *b = f->scratch[5];
    int r = ofs_metadata(f, key, b);
    if (r)
        return r;
    uint32_t comment = ofs_be32(b + f->block_bytes - 72u);
    return comment ? reference(c, comment) : 0;
}
static int cache_reference(void *context, uint32_t key) { return reference(context, key); }
static int process_object(Check *c, uint32_t key) {
    OfsFs *f = c->fs;
    OfsNode n;
    int r = ofs_node(f, key, &n, f->scratch[5]);
    if (r)
        return bad(c, key, r);
    if (n.type != IFS_OFS_ROOT && n.type != IFS_OFS_DIRECTORY && n.type != IFS_OFS_FILE)
        return bad(c, key, IFS_OFS_CORRUPT);
    if (mark(c->objects, key))
        return 0;
    if (key != f->root) {
        r = reference(c, key);
        if (r)
            return r;
    }
    r = push_key(c, key, 0);
    if (r)
        return r;
    r = comment_reference(c, key);
    if (r)
        return r;
    if (n.type == IFS_OFS_ROOT || n.type == IFS_OFS_DIRECTORY) {
        r = ofs_cache_check(f, key, cache_reference, c);
        if (r)
            return bad(c, key, r);
        void *memory = c->frames;
        r = reserve(c, &memory, &c->frame_capacity, c->depth + 1u, sizeof(Frame));
        if (r)
            return r;
        c->frames = memory;
        c->frames[c->depth++] = (Frame){key, {0}};
        return 0;
    }
    uint32_t payload = f->block_bytes - 24u, total = n.bytes ? (n.bytes - 1u) / payload + 1u : 0,
             next = 0;
    if (!total)
        return ofs_check_file(f, &n);
    OfsFileCursor cursor = {0};
    uint8_t *data = f->scratch[4];
    for (uint32_t i = 0; i < total; ++i) {
        uint32_t block;
        r = ofs_pointer(f, key, i, total, &cursor, &block);
        if (r)
            return bad(c, key, r);
        if (i && i % f->hash_size == 0) {
            r = reference(c, cursor.list);
            if (r)
                return r;
        }
        if (i && block != next)
            return bad(c, block, IFS_OFS_CORRUPT);
        r = reference(c, block);
        if (r)
            return r;
        r = ofs_data(f, key, n.bytes, i, block, data);
        if (r)
            return bad(c, block, r);
        next = ofs_word(data, 4);
    }
    return 0;
}
static int namespace_walk(Check *c) {
    OfsFs *f = c->fs;
    int r = process_object(c, f->root);
    if (r)
        return r;
    mark(c->names, f->root);
    while (c->depth) {
        Frame *frame = &c->frames[c->depth - 1u];
        OfsNode n;
        r = ofs_next(f, frame->key, &frame->cursor, &n);
        if (r == IFS_OFS_END) {
            --c->depth;
            continue;
        }
        if (r)
            return bad(c, frame->key, r);
        if (mark(c->names, n.key))
            return bad(c, n.key, IFS_OFS_CORRUPT);
        OfsNode first;
        r = ofs_lookup(f, n.parent, n.name, n.name_bytes, &first);
        if (r)
            return bad(c, n.key, r);
        if (first.key != n.key)
            return bad(c, n.key, IFS_OFS_CORRUPT);
        if (n.type == IFS_OFS_LINKFILE || n.type == IFS_OFS_LINKDIR) {
            r = reference(c, n.key);
            if (r)
                return r;
            r = comment_reference(c, n.key);
            if (r)
                return r;
            r = push_key(c, n.key, 1);
            if (r)
                return r;
            r = process_object(c, n.object);
            if (r)
                return r;
        } else if (n.type == IFS_OFS_SOFTLINK) {
            r = reference(c, n.key);
            if (r)
                return r;
            r = comment_reference(c, n.key);
            if (r)
                return r;
            uint8_t *b = f->scratch[5];
            r = ofs_metadata(f, n.key, b);
            if (r)
                return bad(c, n.key, r);
            uint32_t i = 0, limit = f->block_bytes - 224u;
            while (i < limit && b[24u + i])
                ++i;
            if (i == limit)
                return bad(c, n.key, IFS_OFS_CORRUPT);
        } else {
            r = process_object(c, n.object);
            if (r)
                return r;
        }
    }
    for (size_t i = 0; i < c->object_count; ++i) {
        OfsNode object;
        r = ofs_node(f, c->object_keys[i], &object, f->scratch[5]);
        if (r)
            return bad(c, c->object_keys[i], r);
        if (object.key == f->root)
            continue;
        uint32_t key = object.link;
        while (key) {
            if (mark(c->links, key) || !bit(c->names, key))
                return bad(c, key, IFS_OFS_CORRUPT);
            OfsNode link;
            r = ofs_node(f, key, &link, f->scratch[5]);
            if (r)
                return bad(c, key, r);
            if (link.original != object.key ||
                link.type != (object.type == IFS_OFS_FILE ? IFS_OFS_LINKFILE : IFS_OFS_LINKDIR))
                return bad(c, key, IFS_OFS_CORRUPT);
            key = link.link;
        }
    }
    for (size_t i = 0; i < c->alias_count; ++i)
        if (!bit(c->links, c->alias_keys[i]))
            return bad(c, c->alias_keys[i], IFS_OFS_CORRUPT);
    return 0;
}
static int bitmap_compare(Check *c, int *invalid) {
    OfsFs *f = c->fs;
    uint8_t *b = f->scratch[5];
    for (uint32_t page = 0; page < f->bitmap_count; ++page) {
        uint32_t key;
        int r = ofs_bitmap_key(f, page, &key);
        if (r)
            return bad(c, f->root, r);
        r = ofs_read_block(f, key, b, 0);
        if (r)
            return bad(c, key, r);
        if (ifs_ofs_block_checksum(b, f->block_bytes)) {
            *invalid = 1;
            c->report->first_bad_block = key;
        }
        uint64_t first = (uint64_t)f->reserved + (uint64_t)page * f->bitmap_bits;
        for (uint32_t i = 0; i < f->bitmap_bits && first + i < f->blocks; ++i) {
            int free_bit = (ofs_word(b, 1u + i / 32u) >> (i % 32u)) & 1u;
            int used = bit(c->referenced, (uint32_t)(first + i));
            if (free_bit == used) {
                ++c->report->bitmap_mismatches;
                *invalid = 1;
                if (!c->report->first_bad_block)
                    c->report->first_bad_block = (uint32_t)(first + i);
            }
            if (!used)
                ++c->report->free_blocks;
        }
    }
    return 0;
}
static int rebuild(Check *c) {
    OfsFs *f = c->fs;
    f->bitmap_valid = 1;
    f->failed = 0;
    f->io.flags &= ~IFS_OFS_ALLOW_DIRTY;
    int r = ofs_begin(f);
    if (r)
        return r;
    uint8_t *b = f->scratch[5];
    for (uint32_t page = 0; page < f->bitmap_count; ++page) {
        uint32_t key;
        r = ofs_bitmap_key(f, page, &key);
        if (r)
            return ofs_end(f, r);
        ofs_zero(b, f->block_bytes);
        uint64_t first = (uint64_t)f->reserved + (uint64_t)page * f->bitmap_bits;
        for (uint32_t i = 0; i < f->bitmap_bits && first + i < f->blocks; ++i)
            if (!bit(c->referenced, (uint32_t)(first + i)))
                ofs_setword(b, 1u + i / 32u,
                            ofs_word(b, 1u + i / 32u) | (UINT32_C(1) << (i % 32u)));
        ofs_checksum(f, b, 0);
        r = ofs_write_block(f, key, b);
        if (r)
            return ofs_end(f, r);
    }
    f->used_blocks = (uint32_t)c->report->referenced_blocks;
    return ofs_end(f, 0);
}
int ofs_verify(OfsFs *f, IfsOfsCheckV3 *report, int repair) {
    if (!f->mounted || !report)
        return IFS_OFS_INVALID;
    ofs_zero(report, sizeof(*report));
    report->structure_bytes = sizeof(*report);
    if (repair && (f->io.flags & IFS_OFS_READ_ONLY))
        return IFS_OFS_ACCESS;
    if (!f->io.allocate || !f->io.release)
        return IFS_OFS_NO_MEMORY;
    ofs_invalidate(f);
    int r = ofs_index_bitmaps(f, 0);
    if (r)
        return r;
    Check c = {0};
    c.fs = f;
    c.report = report;
    c.map_bytes = ((size_t)f->blocks + 7u) / 8u;
    if (c.map_bytes > SIZE_MAX / 4u)
        return IFS_OFS_NO_MEMORY;
    c.memory = f->io.allocate(f->io.context, c.map_bytes * 4u);
    if (!c.memory)
        return IFS_OFS_NO_MEMORY;
    ofs_zero(c.memory, c.map_bytes * 4u);
    c.referenced = c.memory;
    c.names = c.memory + c.map_bytes;
    c.objects = c.names + c.map_bytes;
    c.links = c.objects + c.map_bytes;
    for (uint32_t i = 0; i < f->system_count && !r; ++i)
        r = reference(&c, f->system_keys[i]);
    if (!r)
        r = namespace_walk(&c);
    int invalid = !f->bitmap_valid;
    if (!r)
        r = bitmap_compare(&c, &invalid);
    /* A zero LNFS counter is an explicit recalculate sentinel. */
    if (!r && f->variant == 6u) {
        uint8_t *b = f->scratch[5];
        r = ofs_metadata(f, f->root, b);
        uint32_t count = ofs_be32(b + f->block_bytes - 44u);
        if (!r && ((count && count != report->referenced_blocks) || (repair && !count))) {
            invalid = 1;
            if (!report->first_bad_block)
                report->first_bad_block = f->root;
        }
    }
    if (!r && repair && invalid) {
        r = rebuild(&c);
        if (!r)
            invalid = 0;
    }
    if (!r && repair && !invalid) {
        f->io.flags &= ~IFS_OFS_ALLOW_DIRTY;
        f->used_blocks = (uint32_t)report->referenced_blocks;
    }
    if (!r && invalid) {
        report->errors = 1;
        r = IFS_OFS_CORRUPT;
    }
    if (c.frames)
        f->io.release(f->io.context, c.frames);
    if (c.object_keys)
        f->io.release(f->io.context, c.object_keys);
    if (c.alias_keys)
        f->io.release(f->io.context, c.alias_keys);
    f->io.release(f->io.context, c.memory);
    return r;
}
