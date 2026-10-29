/* SPDX-License-Identifier: GPL-3.0-or-later
 * OFS storage algorithms extracted from Linux storage.c and expressed in
 * partition-relative byte I/O. OFS_SPECIFICATION sections 9, 10 and 14.
 * Every data block retains its 24-byte header; FFS raw payload is never used.
 */
#include "ofs_engine.h"

/* Shared admission for public byte APIs and native host adapters. A host must
 * not bypass whole-file validation, write protection or allocation preflight
 * merely because it already holds an inode/object key. */
static int growth_available(OfsFs *f, uint32_t old, uint32_t bytes) {
    if (bytes <= old) return 0;
    uint32_t payload = f->block_bytes - 24u;
    uint64_t a = old ? (old - 1u) / payload + 1u : 0;
    uint64_t b = (bytes - 1u) / payload + 1u;
    uint64_t ae = a ? (a - 1u) / f->hash_size : 0;
    uint64_t be = (b - 1u) / f->hash_size;
    IfsOfsSpaceV3 space;
    int r = ofs_space(f, &space);
    return r ? r : b - a + be - ae <= space.free_bytes / f->block_bytes ? 0 : IFS_OFS_NO_SPACE;
}
int ofs_write_checked(OfsFs *f, OfsNode *n, uint32_t at, const void *data, uint32_t bytes) {
    if (!n || n->object_type != IFS_OFS_FILE || (!data && bytes) || bytes > UINT32_MAX - at)
        return IFS_OFS_INVALID;
    if (n->protection & 4u) return IFS_OFS_ACCESS;
    if (!bytes) return 0;
    int r = ofs_check_file(f, n);
    if (!r) r = growth_available(f, n->bytes, at + bytes);
    if (!r) r = ofs_begin(f);
    return r ? r : ofs_end(f, ofs_write_file(f, n, at, data, bytes));
}
int ofs_resize_checked(OfsFs *f, OfsNode *n, uint32_t bytes) {
    if (!n || n->object_type != IFS_OFS_FILE) return IFS_OFS_INVALID;
    if (n->protection & 4u) return IFS_OFS_ACCESS;
    int r = ofs_check_file(f, n);
    if (!r) r = growth_available(f, n->bytes, bytes);
    if (!r) r = ofs_begin(f);
    return r ? r : ofs_end(f, ofs_resize(f, n, bytes));
}

static uint32_t file_blocks(OfsFs *f, uint32_t bytes) {
    return bytes ? (bytes - 1u) / (f->block_bytes - 24u) + 1u : 0;
}
static int table_read(OfsFs *f, uint32_t owner, uint32_t key, uint8_t *b) {
    if (!ofs_valid_block(f, key))
        return IFS_OFS_CORRUPT;
    int r = ofs_read_block(f, key, b, 0);
    if (r)
        return r;
    if (ofs_word(b, 0) != (key == owner ? 2u : 16u) || ofs_word(b, 1) != key ||
        (int32_t)ofs_be32(b + f->block_bytes - 4u) != IFS_OFS_FILE ||
        ifs_ofs_block_checksum(b, f->block_bytes) ||
        (key != owner && ofs_be32(b + f->block_bytes - 12u) != owner))
        return IFS_OFS_CORRUPT;
    return 0;
}
int ofs_pointer(OfsFs *f, uint32_t owner, uint32_t logical, uint32_t total, OfsFileCursor *c,
                uint32_t *out) {
    if (!out || logical >= total || total > f->blocks - f->reserved - 1u)
        return IFS_OFS_CORRUPT;
    uint32_t want = logical / f->hash_size, key = owner, index = 0;
    if (c && c->owner == owner && c->list && c->list_index <= want) {
        key = c->list;
        index = c->list_index;
    }
    uint8_t *b = f->scratch[3];
    for (;;) {
        int r = table_read(f, owner, key, b);
        if (r)
            return r;
        uint64_t base = (uint64_t)index * f->hash_size;
        if (base >= total)
            return IFS_OFS_CORRUPT;
        uint32_t expected = total - base < f->hash_size ? (uint32_t)(total - base) : f->hash_size;
        if (ofs_word(b, 2) != expected)
            return IFS_OFS_CORRUPT;
        if (index == want)
            break;
        uint32_t next = ofs_be32(b + f->block_bytes - 8u);
        if (next == key)
            return IFS_OFS_CORRUPT;
        key = next;
        ++index;
    }
    uint32_t k = ofs_word(b, 6u + f->hash_size - 1u - logical % f->hash_size);
    if (!ofs_valid_block(f, k) || k == owner || k == key)
        return IFS_OFS_CORRUPT;
    if (!logical && ofs_word(b, 4) != k)
        return IFS_OFS_CORRUPT;
    if (want == (total - 1u) / f->hash_size && ofs_be32(b + f->block_bytes - 8u))
        return IFS_OFS_CORRUPT;
    if (c) {
        c->owner = owner;
        c->list = key;
        c->list_index = index;
    }
    *out = k;
    return 0;
}
int ofs_data(OfsFs *f, uint32_t owner, uint32_t bytes, uint32_t logical, uint32_t key, uint8_t *b) {
    int r = ofs_read_block(f, key, b, 1);
    if (r)
        return r;
    uint32_t payload = f->block_bytes - 24u;
    uint64_t base = (uint64_t)logical * payload;
    if (base >= bytes)
        return IFS_OFS_CORRUPT;
    uint32_t expected = bytes - base < payload ? (uint32_t)(bytes - base) : payload;
    if (ofs_word(b, 0) != 8u || ofs_word(b, 1) != owner || ofs_word(b, 2) != logical + 1u ||
        ofs_word(b, 3) != expected || ifs_ofs_block_checksum(b, f->block_bytes))
        return IFS_OFS_CORRUPT;
    uint32_t next = ofs_word(b, 4);
    if (base + expected == bytes ? next != 0 : !ofs_valid_block(f, next))
        return IFS_OFS_CORRUPT;
    return 0;
}
int ofs_read_file(OfsFs *f, const OfsNode *n, uint32_t offset, void *buffer, uint32_t count,
                  OfsFileCursor *cursor) {
    if (!n || n->object_type != IFS_OFS_FILE || (!buffer && count) || offset > n->bytes ||
        count > n->bytes - offset)
        return IFS_OFS_INVALID;
    if (n->protection & 8u)
        return IFS_OFS_ACCESS;
    OfsFileCursor local = {0};
    OfsFileCursor *c = cursor ? cursor : &local;
    if (c->owner != n->object || c->size != n->bytes) {
        ofs_zero(c, sizeof(*c));
        c->owner = n->object;
        c->size = n->bytes;
    }
    uint32_t payload = f->block_bytes - 24u, total = file_blocks(f, n->bytes), done = 0;
    uint8_t *out = buffer, *b = f->scratch[4];
    while (done < count) {
        uint32_t at = offset + done, logical = at / payload, key;
        int r = ofs_pointer(f, n->object, logical, total, c, &key);
        if (r)
            return r;
        if (c->sequence == logical && c->sequence && c->next != key)
            return IFS_OFS_CORRUPT;
        r = ofs_data(f, n->object, n->bytes, logical, key, b);
        if (r)
            return r;
        uint32_t within = at % payload, part = payload - within;
        if (part > count - done)
            part = count - done;
        ofs_copy(out + done, b + 24u + within, part);
        done += part;
        c->sequence = logical + 1u;
        c->block = key;
        c->next = ofs_word(b, 4);
    }
    return 0;
}
int ofs_check_file(OfsFs *f, const OfsNode *n) {
    uint8_t *b = f->scratch[4];
    uint32_t total = file_blocks(f, n->bytes), next = 0;
    OfsFileCursor c = {0};
    c.owner = n->object;
    if (!total) {
        int r = table_read(f, n->object, n->object, b);
        if (r)
            return r;
        if (ofs_word(b, 2) || ofs_word(b, 4) || ofs_be32(b + f->block_bytes - 8u))
            return IFS_OFS_CORRUPT;
        for (uint32_t i = 0; i < f->hash_size; ++i)
            if (ofs_word(b, 6u + i))
                return IFS_OFS_CORRUPT;
    }
    for (uint32_t i = 0; i < total; ++i) {
        uint32_t key;
        int r = ofs_pointer(f, n->object, i, total, &c, &key);
        if (r)
            return r;
        if (i && key != next)
            return IFS_OFS_CORRUPT;
        r = ofs_data(f, n->object, n->bytes, i, key, b);
        if (r)
            return r;
        next = ofs_word(b, 4);
    }
    return 0;
}
static int set_size(OfsFs *f, OfsNode *n, uint32_t bytes) {
    uint8_t *b = f->scratch[3];
    int r = table_read(f, n->object, n->object, b);
    if (r)
        return r;
    ofs_put32(b + f->block_bytes - 188u, bytes);
    ofs_checksum(f, b, 5);
    r = ofs_write_block(f, n->object, b);
    if (!r)
        n->bytes = bytes;
    return r;
}
static int grow(OfsFs *f, OfsNode *n, uint32_t bytes) {
    uint32_t payload = f->block_bytes - 24u;
    OfsFileCursor cursor = {0};
    cursor.owner = n->object;
    while (n->bytes < bytes) {
        uint32_t total = file_blocks(f, n->bytes), used = n->bytes % payload, previous = 0;
        uint8_t *data = f->scratch[4], *table = f->scratch[3];
        int r;
        if (total) {
            r = ofs_pointer(f, n->object, total - 1u, total, &cursor, &previous);
            if (r)
                return r;
        }
        if (used) {
            r = ofs_data(f, n->object, n->bytes, total - 1u, previous, data);
            if (r)
                return r;
            uint32_t add = payload - used;
            if (add > bytes - n->bytes)
                add = bytes - n->bytes;
            ofs_zero(data + 24u + used, add);
            ofs_setword(data, 3, used + add);
            ofs_checksum(f, data, 5);
            r = ofs_write_block(f, previous, data);
            if (r)
                return r;
            r = set_size(f, n, n->bytes + add);
            if (r)
                return r;
            continue;
        }
        uint32_t list = n->object, within = total % f->hash_size;
        if (total >= f->hash_size) {
            list = cursor.list;
            if (!within) {
                uint32_t new_list;
                r = ofs_allocate(f, &new_list);
                if (r)
                    return r;
                uint8_t *fresh = f->scratch[5];
                ofs_zero(fresh, f->block_bytes);
                ofs_setword(fresh, 0, 16);
                ofs_setword(fresh, 1, new_list);
                ofs_put32(fresh + f->block_bytes - 12u, n->object);
                ofs_put32(fresh + f->block_bytes - 4u, (uint32_t)IFS_OFS_FILE);
                ofs_checksum(f, fresh, 5);
                r = ofs_write_block(f, new_list, fresh);
                if (r)
                    return r;
                r = table_read(f, n->object, list, table);
                if (r)
                    return r;
                if (ofs_be32(table + f->block_bytes - 8u))
                    return IFS_OFS_CORRUPT;
                ofs_put32(table + f->block_bytes - 8u, new_list);
                ofs_checksum(f, table, 5);
                r = ofs_write_block(f, list, table);
                if (r)
                    return r;
                list = new_list;
                cursor.list = new_list;
                cursor.list_index = total / f->hash_size;
            }
        }
        uint32_t key;
        r = ofs_allocate(f, &key);
        if (r)
            return r;
        uint32_t add = bytes - n->bytes < payload ? bytes - n->bytes : payload;
        ofs_zero(data, f->block_bytes);
        ofs_setword(data, 0, 8);
        ofs_setword(data, 1, n->object);
        ofs_setword(data, 2, total + 1u);
        ofs_setword(data, 3, add);
        ofs_checksum(f, data, 5);
        r = ofs_write_block(f, key, data);
        if (r)
            return r;
        r = table_read(f, n->object, list, table);
        if (r)
            return r;
        if (ofs_word(table, 2) != within || ofs_word(table, 6u + f->hash_size - 1u - within))
            return IFS_OFS_CORRUPT;
        ofs_setword(table, 6u + f->hash_size - 1u - within, key);
        ofs_setword(table, 2, within + 1u);
        if (!total)
            ofs_setword(table, 4, key);
        ofs_checksum(f, table, 5);
        r = ofs_write_block(f, list, table);
        if (r)
            return r;
        if (total) {
            r = ofs_data(f, n->object, n->bytes, total - 1u, previous, data);
            if (r)
                return r;
            ofs_setword(data, 4, key);
            ofs_checksum(f, data, 5);
            r = ofs_write_block(f, previous, data);
            if (r)
                return r;
        }
        r = set_size(f, n, n->bytes + add);
        if (r)
            return r;
    }
    return 0;
}
static int shrink(OfsFs *f, OfsNode *n, uint32_t bytes) {
    uint32_t payload = f->block_bytes - 24u, old = file_blocks(f, n->bytes),
             keep = file_blocks(f, bytes), first_free = 0, list = n->object;
    uint8_t *data = f->scratch[4], *table = f->scratch[3];
    OfsFileCursor c = {0};
    int r;
    if (keep) {
        uint32_t key;
        r = ofs_pointer(f, n->object, keep - 1u, old, &c, &key);
        if (r)
            return r;
        list = c.list;
        r = ofs_data(f, n->object, n->bytes, keep - 1u, key, data);
        if (r)
            return r;
        first_free = ofs_word(data, 4);
        uint32_t tail = bytes - (keep - 1u) * payload;
        ofs_zero(data + 24u + tail, payload - tail);
        ofs_setword(data, 3, tail);
        ofs_setword(data, 4, 0);
        ofs_checksum(f, data, 5);
        r = ofs_write_block(f, key, data);
        if (r)
            return r;
    } else if (old) {
        r = ofs_pointer(f, n->object, 0, old, &c, &first_free);
        if (r)
            return r;
    }
    r = table_read(f, n->object, list, table);
    if (r)
        return r;
    uint32_t free_list = ofs_be32(table + f->block_bytes - 8u);
    uint32_t within = keep ? (keep - 1u) % f->hash_size + 1u : 0;
    for (uint32_t i = within; i < f->hash_size; ++i)
        ofs_setword(table, 6u + f->hash_size - 1u - i, 0);
    ofs_setword(table, 2, within);
    ofs_put32(table + f->block_bytes - 8u, 0);
    if (!keep)
        ofs_setword(table, 4, 0);
    ofs_checksum(f, table, 5);
    r = ofs_write_block(f, list, table);
    if (r)
        return r;
    uint32_t old_bytes = n->bytes;
    r = set_size(f, n, bytes);
    if (r)
        return r;
    uint32_t key = first_free;
    for (uint32_t i = keep; i < old; ++i) {
        if (!key)
            return IFS_OFS_CORRUPT;
        r = ofs_data(f, n->object, old_bytes, i, key, data);
        if (r)
            return r;
        uint32_t next = ofs_word(data, 4);
        r = ofs_bitmap_set(f, key, 1);
        if (r)
            return r;
        key = next;
    }
    if (key)
        return IFS_OFS_CORRUPT;
    uint32_t budget = old / f->hash_size + 1u;
    while (free_list) {
        if (!budget--)
            return IFS_OFS_CORRUPT;
        r = table_read(f, n->object, free_list, table);
        if (r)
            return r;
        uint32_t next = ofs_be32(table + f->block_bytes - 8u);
        r = ofs_bitmap_set(f, free_list, 1);
        if (r)
            return r;
        free_list = next;
    }
    return 0;
}
static int changed(OfsFs *f, OfsNode *n) {
    uint8_t *b = f->scratch[3];
    int r = table_read(f, n->object, n->object, b);
    if (r)
        return r;
    ofs_put32(b + f->block_bytes - 192u, ofs_be32(b + f->block_bytes - 192u) & ~16u);
    IfsOfsDate now;
    r = ofs_clock(f, &now);
    if (r == 0)
        ofs_put_date(b + f->block_bytes - (f->variant == 6u ? 60u : 92u), &now);
    else if (r != IFS_OFS_NOT_FOUND)
        return r;
    ofs_checksum(f, b, 5);
    r = ofs_write_block(f, n->object, b);
    if (!r && ofs_be32(b + f->block_bytes - 12u) == f->root)
        r = ofs_touch_root(f, 1);
    if (!r)
        r = ofs_cache_object(f, n->object);
    return r;
}
int ofs_resize(OfsFs *f, OfsNode *n, uint32_t bytes) {
    if (!n || n->object_type != IFS_OFS_FILE)
        return IFS_OFS_INVALID;
    if (n->protection & 4u)
        return IFS_OFS_ACCESS;
    if (!f->mutation)
        return IFS_OFS_INVALID;
    int r = bytes > n->bytes ? grow(f, n, bytes) : bytes < n->bytes ? shrink(f, n, bytes) : 0;
    return r ? r : changed(f, n);
}
int ofs_write_file(OfsFs *f, OfsNode *n, uint32_t offset, const void *source, uint32_t count) {
    if (!n || n->object_type != IFS_OFS_FILE || (!source && count) || count > UINT32_MAX - offset)
        return IFS_OFS_INVALID;
    if (n->protection & 4u)
        return IFS_OFS_ACCESS;
    if (!count)
        return 0;
    uint32_t end = offset + count;
    int r = 0;
    if (end > n->bytes) {
        r = grow(f, n, end);
        if (r)
            return r;
    }
    uint32_t payload = f->block_bytes - 24u, total = file_blocks(f, n->bytes), done = 0;
    OfsFileCursor c = {0};
    uint8_t *b = f->scratch[4];
    const uint8_t *src = source;
    while (done < count) {
        uint32_t at = offset + done, logical = at / payload, key;
        r = ofs_pointer(f, n->object, logical, total, &c, &key);
        if (r)
            return r;
        r = ofs_data(f, n->object, n->bytes, logical, key, b);
        if (r)
            return r;
        uint32_t part = payload - at % payload;
        if (part > count - done)
            part = count - done;
        ofs_copy(b + 24u + at % payload, src + done, part);
        ofs_checksum(f, b, 5);
        r = ofs_write_block(f, key, b);
        if (r)
            return r;
        done += part;
    }
    return changed(f, n);
}
