/* SPDX-License-Identifier: GPL-3.0-or-later
 * Portable mutations adapted from the existing OFS Linux namespace adapter.
 * Hard-link promotion deliberately retains the original object's block key,
 * so OFS data owners and other aliases never need speculative retargeting.
 * OFS_SPECIFICATION sections 8, 11, 13 and 14.
 */
#include "ofs_engine.h"

static int touch_directory(OfsFs *f, uint32_t key) {
    uint8_t *b = f->scratch[5];
    int r = ofs_directory(f, key, b);
    if (r)
        return r;
    uint32_t parent = key == f->root ? 0 : ofs_be32(b + f->block_bytes - 12u);
    IfsOfsDate now;
    r = ofs_clock(f, &now);
    if (r == 0) {
        ofs_put_date(b + f->block_bytes - (f->variant == 6u && key != f->root ? 60u : 92u), &now);
        ofs_checksum(f, b, 5);
        r = ofs_write_block(f, key, b);
    } else if (r == IFS_OFS_NOT_FOUND)
        r = 0;
    if (!r)
        r = ofs_cache_queue(f, key);
    if (!r && parent)
        r = ofs_cache_queue(f, parent);
    return r;
}
static int insert_hash(OfsFs *f, uint32_t key, uint32_t parent, const uint8_t *name,
                       uint32_t length) {
    uint8_t *dir = f->scratch[0], *entry = f->scratch[1];
    uint32_t bucket = ofs_hash(f, name, length);
    int r = ofs_directory(f, parent, dir);
    if (r)
        return r;
    r = ofs_metadata(f, key, entry);
    if (r)
        return r;
    ofs_put32(entry + f->block_bytes - 12u, parent);
    ofs_put32(entry + f->block_bytes - 16u, ofs_word(dir, 6u + bucket));
    ofs_checksum(f, entry, 5);
    r = ofs_write_block(f, key, entry);
    if (r)
        return r;
    ofs_setword(dir, 6u + bucket, key);
    ofs_checksum(f, dir, 5);
    r = ofs_write_block(f, parent, dir);
    return r ? r : touch_directory(f, parent);
}
static int remove_hash(OfsFs *f, const OfsNode *n) {
    uint32_t bucket = ofs_hash(f, n->name, n->name_bytes), key = n->parent, seen = 0;
    uint8_t *b = f->scratch[0];
    int r = ofs_directory(f, key, b);
    if (r)
        return r;
    for (;;) {
        uint32_t offset = key == n->parent ? 24u + 4u * bucket : f->block_bytes - 16u;
        uint32_t next = ofs_be32(b + offset);
        if (next == n->key) {
            if (n->chain == n->key)
                return IFS_OFS_CORRUPT;
            ofs_put32(b + offset, n->chain);
            ofs_checksum(f, b, 5);
            r = ofs_write_block(f, key, b);
            return r ? r : touch_directory(f, n->parent);
        }
        if (!next || ++seen > f->blocks - f->reserved)
            return IFS_OFS_CORRUPT;
        key = next;
        r = ofs_metadata(f, key, b);
        if (r)
            return r;
        if (ofs_be32(b + f->block_bytes - 12u) != n->parent)
            return IFS_OFS_CORRUPT;
    }
}
static int directory_validate(OfsFs *f, uint32_t key, int empty) {
    OfsDirCursor c = {0};
    OfsNode n;
    int r;
    while ((r = ofs_next(f, key, &c, &n)) == 0) {
        if (empty)
            return IFS_OFS_ACCESS;
        OfsNode first;
        r = ofs_lookup(f, key, n.name, n.name_bytes, &first);
        if (r)
            return r;
        if (first.key != n.key)
            return IFS_OFS_CORRUPT;
    }
    return r == IFS_OFS_END ? ofs_cache_check(f, key, 0, 0) : r;
}
static int available(OfsFs *f, uint64_t needed) {
    IfsOfsSpaceV3 s;
    int r = ofs_space(f, &s);
    if (r)
        return r;
    return needed <= s.free_bytes / f->block_bytes ? 0 : IFS_OFS_NO_SPACE;
}
static int parent_writable(OfsFs *f, uint32_t key) {
    OfsNode p;
    int r = ofs_node(f, key, &p, f->scratch[1]);
    if (r)
        return r;
    if (p.protection & 4u)
        return IFS_OFS_ACCESS;
    return directory_validate(f, key, 0);
}
int ofs_create(OfsFs *f, const char *path, int32_t type, const char *target, uint32_t original) {
    uint8_t name[108];
    uint32_t length, parent;
    int r = ofs_parent(f, path, &parent, name, &length);
    if (r)
        return r;
    OfsNode exists;
    r = ofs_lookup(f, parent, name, length, &exists);
    if (r == 0)
        return IFS_OFS_EXISTS;
    if (r != IFS_OFS_NOT_FOUND)
        return r;
    if (type != IFS_OFS_FILE && type != IFS_OFS_DIRECTORY && type != IFS_OFS_SOFTLINK &&
        type != IFS_OFS_LINKFILE && type != IFS_OFS_LINKDIR)
        return IFS_OFS_INVALID;
    r = parent_writable(f, parent);
    if (r)
        return r;
    r = available(f, 1u + (f->variant == 4u && type == IFS_OFS_DIRECTORY));
    if (r)
        return r;
    uint32_t target_length = 0;
    if (type == IFS_OFS_SOFTLINK) {
        r = ofs_latin1(target, f->scratch[4], f->block_bytes - 225u, &target_length, 0);
        if (r || !target_length)
            return IFS_OFS_INVALID;
        for (uint32_t i = 0; i < target_length; ++i)
            if (f->scratch[4][i] < 32u)
                return IFS_OFS_INVALID;
    }
    if (type == IFS_OFS_LINKFILE || type == IFS_OFS_LINKDIR) {
        r = ofs_node(f, original, &exists, f->scratch[1]);
        if (r)
            return r;
        if (exists.type != (type == IFS_OFS_LINKFILE ? IFS_OFS_FILE : IFS_OFS_DIRECTORY))
            return IFS_OFS_INVALID;
        r = type == IFS_OFS_LINKFILE ? ofs_check_file(f, &exists)
                                   : directory_validate(f, exists.key, 0);
        if (r)
            return r;
    }
    IfsOfsDate now = {0};
    r = ofs_clock(f, &now);
    if (r != 0 && r != IFS_OFS_NOT_FOUND)
        return r;
    r = ofs_begin(f);
    if (r)
        return r;
    uint32_t key;
    r = ofs_allocate(f, &key);
    if (r)
        return ofs_end(f, r);
    uint32_t cache = 0;
    if (type == IFS_OFS_DIRECTORY) {
        r = ofs_cache_init(f, key, &cache);
        if (r)
            return ofs_end(f, r);
    }
    uint8_t *b = f->scratch[1];
    ofs_zero(b, f->block_bytes);
    ofs_setword(b, 0, 2);
    ofs_setword(b, 1, key);
    if (type == IFS_OFS_SOFTLINK)
        ofs_copy(b + 24u, f->scratch[4], target_length);
    ofs_put32(b + f->block_bytes - 12u, parent);
    ofs_put32(b + f->block_bytes - 4u, (uint32_t)type);
    ofs_put_date(b + f->block_bytes - (f->variant == 6u ? 60u : 92u), &now);
    if (type == IFS_OFS_DIRECTORY)
        ofs_put32(b + f->block_bytes - 8u, cache);
    r = ofs_put_strings(f, key, b, name, length, 0, 0);
    if (r)
        return ofs_end(f, r);
    if (type == IFS_OFS_LINKFILE || type == IFS_OFS_LINKDIR) {
        ofs_put32(b + f->block_bytes - 44u, original);
        ofs_put32(b + f->block_bytes - 40u, exists.link);
    }
    ofs_checksum(f, b, 5);
    r = ofs_write_block(f, key, b);
    if (!r && (type == IFS_OFS_LINKFILE || type == IFS_OFS_LINKDIR)) {
        r = ofs_metadata(f, original, b);
        if (!r) {
            ofs_put32(b + f->block_bytes - 40u, key);
            ofs_checksum(f, b, 5);
            r = ofs_write_block(f, original, b);
        }
    }
    if (!r)
        r = insert_hash(f, key, parent, name, length);
    return ofs_end(f, r);
}
static int remove_alias(OfsFs *f, const OfsNode *n) {
    uint32_t key = n->object, seen = 0;
    uint8_t *b = f->scratch[0];
    int r;
    for (;;) {
        r = ofs_metadata(f, key, b);
        if (r)
            return r;
        uint32_t next = ofs_be32(b + f->block_bytes - 40u);
        if (next == n->key) {
            ofs_put32(b + f->block_bytes - 40u, n->link);
            ofs_checksum(f, b, 5);
            r = ofs_write_block(f, key, b);
            if (r)
                return r;
            break;
        }
        if (!next || next == key || ++seen > f->blocks - f->reserved)
            return IFS_OFS_CORRUPT;
        OfsNode link;
        r = ofs_node(f, next, &link, f->scratch[1]);
        if (r)
            return r;
        if (link.original != n->object ||
            (link.type != IFS_OFS_LINKFILE && link.type != IFS_OFS_LINKDIR))
            return IFS_OFS_CORRUPT;
        key = next;
    }
    r = remove_hash(f, n);
    if (!r)
        r = ofs_drop_comment(f, n->key);
    return r ? r : ofs_bitmap_set(f, n->key, 1);
}
static int parent_cycle(OfsFs *f, uint32_t object, uint32_t parent);
/* An explicit directory hard link may point into its own tree. Such aliases
 * are finite on disk and are scanned once per original object. Promotion,
 * however, must retain an original-parent route to the root: moving the
 * original into its own descendant would strand the entire tree. Select a
 * rooted alias before writes, including when a cyclic alias is newest. */
static int promotion(OfsFs *f, const OfsNode *n, OfsNode *chosen, uint32_t *previous) {
    uint32_t key = n->link, before = n->key, visited = 0;
    while (key) {
        if (++visited > f->blocks - f->reserved)
            return IFS_OFS_CORRUPT;
        int r = ofs_node(f, key, chosen, f->scratch[1]);
        if (r)
            return r;
        if (chosen->original != n->key ||
            chosen->type != (n->type == IFS_OFS_DIRECTORY ? IFS_OFS_LINKDIR : IFS_OFS_LINKFILE))
            return IFS_OFS_CORRUPT;
        r = n->type == IFS_OFS_DIRECTORY ? parent_cycle(f, n->key, chosen->parent) : 0;
        if (!r) {
            *previous = before;
            return 0;
        }
        if (r != IFS_OFS_ACCESS)
            return r;
        before = key;
        key = chosen->link;
    }
    return IFS_OFS_ACCESS;
}
static int unlink_node(OfsFs *f, OfsNode *n) {
    if (n->key == f->root || n->protection & 1u)
        return IFS_OFS_ACCESS;
    if (n->type == IFS_OFS_LINKFILE || n->type == IFS_OFS_LINKDIR)
        return remove_alias(f, n);
    if (n->link) {
        OfsNode promoted;
        uint32_t previous;
        int r = promotion(f, n, &promoted, &previous);
        if (r)
            return r;
        uint32_t remaining = n->link;
        if (previous == n->key)
            remaining = promoted.link;
        else {
            uint8_t *link = f->scratch[1];
            r = ofs_metadata(f, previous, link);
            if (r)
                return r;
            ofs_put32(link + f->block_bytes - 40u, promoted.link);
            ofs_checksum(f, link, 5);
            r = ofs_write_block(f, previous, link);
            if (r)
                return r;
        }
        uint32_t alias_comment = 0;
        if (f->variant == 6u) {
            r = ofs_metadata(f, promoted.key, f->scratch[3]);
            if (r)
                return r;
            alias_comment = ofs_be32(f->scratch[3] + f->block_bytes - 72u);
        }
        r = remove_hash(f, n);
        if (r)
            return r;
        r = remove_hash(f, &promoted);
        if (r)
            return r;
        uint8_t *b = f->scratch[1];
        r = ofs_metadata(f, n->key, b);
        if (r)
            return r;
        /* Reuse the removed alias header if promotion needs a new comment
         * block. Deletion must work even when there are no free blocks. */
        int recycled = f->variant == 6u && promoted.name_bytes + n->comment_bytes + 2u > 112u &&
                       !ofs_be32(b + f->block_bytes - 72u);
        if (recycled)
            ofs_put32(b + f->block_bytes - 72u, promoted.key);
        r = ofs_put_strings(f, n->key, b, promoted.name, promoted.name_bytes, n->comment,
                            n->comment_bytes);
        if (r)
            return r;
        ofs_put32(b + f->block_bytes - 40u, remaining);
        ofs_checksum(f, b, 5);
        r = ofs_write_block(f, n->key, b);
        if (r)
            return r;
        r = insert_hash(f, n->key, promoted.parent, promoted.name, promoted.name_bytes);
        if (r)
            return r;
        if (alias_comment)
            r = ofs_bitmap_set(f, alias_comment, 1);
        return r || recycled ? r : ofs_bitmap_set(f, promoted.key, 1);
    }
    int r = 0;
    if (n->type == IFS_OFS_FILE) {
        /* Amiga delete protection is independent of write protection. */
        n->protection &= ~4u;
        r = ofs_resize(f, n, 0);
    }
    if (r)
        return r;
    if (n->type == IFS_OFS_DIRECTORY)
        r = ofs_cache_drop(f, n->key);
    if (!r)
        r = ofs_drop_comment(f, n->key);
    if (!r)
        r = remove_hash(f, n);
    return r ? r : ofs_bitmap_set(f, n->key, 1);
}
static int can_unlink(OfsFs *f, OfsNode *n) {
    if (n->key == f->root || n->protection & 1u)
        return IFS_OFS_ACCESS;
    int r = parent_writable(f, n->parent);
    if (r)
        return r;
    if (n->type == IFS_OFS_DIRECTORY) {
        r = directory_validate(f, n->object, !n->link);
        if (r || !n->link)
            return r;
        OfsNode chosen;
        uint32_t previous;
        return promotion(f, n, &chosen, &previous);
    }
    if (n->object_type == IFS_OFS_FILE)
        return ofs_check_file(f, n);
    return 0;
}
int ofs_is_orphan(const OfsFs *f, uint32_t key) {
    for (const OfsOrphan *o = f->orphans; o; o = o->next)
        if (o->key == key) return 1;
    return 0;
}
static int orphan_detach(OfsFs *f, OfsNode *n, OfsOrphan *o) {
    int r = remove_hash(f, n);
    if (r) return r;
    o->key = n->object; o->next = f->orphans; f->orphans = o;
    uint8_t *b = f->scratch[1];
    r = ofs_metadata(f, n->object, b);
    if (!r) {
        /* A removed parent may itself be deleted while the handle is open.
         * Orphans have no namespace membership or DOS4 cache record. */
        ofs_put32(b + f->block_bytes - 12u, f->root);
        ofs_put32(b + f->block_bytes - 16u, 0);
        ofs_checksum(f, b, 5);
        r = ofs_write_block(f, n->object, b);
    }
    return r;
}
static int unlink_path(OfsFs *f, const char *path, int deferred) {
    OfsNode n;
    int r = ofs_path(f, path, 0, &n);
    if (r)
        return r;
    r = can_unlink(f, &n);
    if (r)
        return r;
    OfsOrphan *o = 0;
    if (deferred && !n.link && (n.type == IFS_OFS_FILE || n.type == IFS_OFS_DIRECTORY ||
                               n.type == IFS_OFS_SOFTLINK)) {
        o = f->io.allocate(f->io.context, sizeof(*o));
        if (!o) return IFS_OFS_NO_MEMORY;
    }
    r = ofs_begin(f);
    if (r) {
        if (o) f->io.release(f->io.context, o);
        return r;
    }
    r = o ? orphan_detach(f, &n, o) : unlink_node(f, &n);
    if (o && !ofs_is_orphan(f, n.object)) f->io.release(f->io.context, o);
    return ofs_end(f, r);
}
int ofs_unlink(OfsFs *f, const char *path) { return unlink_path(f, path, 0); }
int ofs_unlink_deferred(OfsFs *f, const char *path) { return unlink_path(f, path, 1); }
int ofs_release_orphan(OfsFs *f, uint32_t key) {
    OfsOrphan **at = &f->orphans;
    while (*at && (*at)->key != key) at = &(*at)->next;
    if (!*at) return IFS_OFS_NOT_FOUND;
    OfsNode n;
    int r = ofs_node(f, key, &n, f->scratch[1]);
    if (!r && (n.link || (n.type != IFS_OFS_FILE && n.type != IFS_OFS_DIRECTORY &&
                          n.type != IFS_OFS_SOFTLINK))) r = IFS_OFS_CORRUPT;
    if (!r) r = ofs_begin(f);
    if (r) return r;
    n.protection &= ~4u;
    if (n.type == IFS_OFS_FILE) r = ofs_resize(f, &n, 0);
    else if (n.type == IFS_OFS_DIRECTORY) r = ofs_cache_drop(f, n.key);
    if (!r) r = ofs_drop_comment(f, key);
    if (!r) r = ofs_bitmap_set(f, key, 1);
    if (!r) {
        OfsOrphan *old = *at; *at = old->next;
        f->io.release(f->io.context, old);
    }
    return ofs_end(f, r);
}
static int parent_cycle(OfsFs *f, uint32_t object, uint32_t parent) {
    for (uint32_t i = 0; i < f->blocks - f->reserved; ++i) {
        if (parent == object)
            return IFS_OFS_ACCESS;
        if (parent == f->root)
            return 0;
        OfsNode n;
        int r = ofs_node(f, parent, &n, f->scratch[1]);
        if (r)
            return r;
        if (n.type != IFS_OFS_DIRECTORY || n.parent == parent)
            return IFS_OFS_CORRUPT;
        parent = n.parent;
    }
    return IFS_OFS_CORRUPT;
}
static int rename_path(OfsFs *f, const char *from, const char *to, int deferred) {
    OfsNode n, dest;
    uint8_t name[108];
    uint32_t length, parent;
    int r = ofs_path(f, from, 0, &n);
    if (r)
        return r;
    if (n.key == f->root || n.protection & 1u)
        return IFS_OFS_ACCESS;
    r = ofs_parent(f, to, &parent, name, &length);
    if (r)
        return r;
    r = parent_writable(f, n.parent);
    if (r)
        return r;
    if (parent != n.parent) {
        r = parent_writable(f, parent);
        if (r)
            return r;
    }
    if (n.type == IFS_OFS_DIRECTORY) {
        r = parent_cycle(f, n.object, parent);
        if (r)
            return r;
    }
    int present = ofs_lookup(f, parent, name, length, &dest);
    if (present != 0 && present != IFS_OFS_NOT_FOUND)
        return present;
    if (!present && dest.key != n.key) {
        if (dest.object == n.object)
            return 0;
        int source_dir = n.object_type == IFS_OFS_DIRECTORY,
            dest_dir = dest.object_type == IFS_OFS_DIRECTORY;
        if (source_dir != dest_dir)
            return IFS_OFS_INVALID;
        r = can_unlink(f, &dest);
        if (r)
            return r;
    }
    if (!present && dest.key == n.key && parent == n.parent && length == n.name_bytes &&
        ofs_equal(name, n.name, length))
        return 0;
    if (f->variant == 6u && length + n.comment_bytes + 2u > 112u) {
        r = ofs_metadata(f, n.key, f->scratch[3]);
        if (r)
            return r;
        if (!ofs_be32(f->scratch[3] + f->block_bytes - 72u))
            r = available(f, 1);
        if (r)
            return r;
    }
    OfsOrphan *o = 0;
    if (deferred && !present && dest.key != n.key && !dest.link &&
        (dest.type == IFS_OFS_FILE || dest.type == IFS_OFS_DIRECTORY || dest.type == IFS_OFS_SOFTLINK)) {
        o = f->io.allocate(f->io.context, sizeof(*o));
        if (!o) return IFS_OFS_NO_MEMORY;
    }
    r = ofs_begin(f);
    if (r) {
        if (o) f->io.release(f->io.context, o);
        return r;
    }
    if (!present && dest.key != n.key) {
        r = o ? orphan_detach(f, &dest, o) : unlink_node(f, &dest);
        if (o && !ofs_is_orphan(f, dest.object)) f->io.release(f->io.context, o);
        if (r)
            return ofs_end(f, r);
    }
    /* Destination deletion can alter this entry's predecessor/hash chain. */
    r = ofs_node(f, n.key, &n, f->scratch[1]);
    if (r)
        return ofs_end(f, r);
    r = remove_hash(f, &n);
    if (r)
        return ofs_end(f, r);
    uint8_t *b = f->scratch[1];
    r = ofs_metadata(f, n.key, b);
    if (!r) {
        r = ofs_put_strings(f, n.key, b, name, length, n.comment, n.comment_bytes);
        if (!r) {
            ofs_checksum(f, b, 5);
            r = ofs_write_block(f, n.key, b);
        }
    }
    if (!r)
        r = insert_hash(f, n.key, parent, name, length);
    return ofs_end(f, r);
}
int ofs_rename(OfsFs *f, const char *from, const char *to) { return rename_path(f, from, to, 0); }
int ofs_rename_deferred(OfsFs *f, const char *from, const char *to) { return rename_path(f, from, to, 1); }
/* Metadata operation selectors are private to the V3 adapter. */
int ofs_set_metadata_key(OfsFs *f, uint32_t key_in, uint32_t operation, const void *value) {
    OfsNode n;
    int r = ofs_node(f, key_in, &n, f->scratch[1]);
    if (r)
        return r;
    if (n.object != n.key) {
        r = ofs_node(f, n.object, &n, f->scratch[1]);
        if (r)
            return r;
    }
    uint8_t comment[80];
    uint32_t length = 0;
    if (!value)
        return IFS_OFS_INVALID;
    if (operation == 3u) {
        r = ofs_latin1(value, comment, 79, &length, 0);
        if (r)
            return r;
    }
    if (operation >= 4u) {
        uint32_t mask = operation >> 8;
        if (!ofs_date_valid(value) || !mask || (mask & ~7u) || (n.key != f->root && mask != 1u))
            return IFS_OFS_INVALID;
    } else if (n.key == f->root)
        return IFS_OFS_INVALID;
    if (operation == 3u && f->variant == 6u && n.name_bytes + length + 2u > 112u) {
        r = ofs_metadata(f, n.key, f->scratch[3]);
        if (r)
            return r;
        if (!ofs_be32(f->scratch[3] + f->block_bytes - 72u))
            r = available(f, 1);
        if (r)
            return r;
    }
    r = ofs_begin(f);
    if (r)
        return r;
    if (n.key == f->root && operation >= 4u && ((operation >> 8) & 2u))
        f->preserve_volume_date = 1;
    uint32_t key = n.object;
    uint8_t *b = f->scratch[1];
    r = ofs_metadata(f, key, b);
    if (!r) {
        if (operation == 1u)
            ofs_put32(b + f->block_bytes - 192u, *(const uint32_t *)value);
        else if (operation == 2u) {
            const uint16_t *ids = value;
            ofs_put16(b + f->block_bytes - 196u, ids[0]);
            ofs_put16(b + f->block_bytes - 194u, ids[1]);
        } else if (operation == 3u) {
            r = ofs_put_strings(f, key, b, n.name, n.name_bytes, comment, length);
        } else {
            uint32_t mask = operation >> 8;
            if (mask & 1u)
                ofs_put_date(b + f->block_bytes - (f->variant == 6u && key != f->root ? 60u : 92u),
                             value);
            if (mask & 2u)
                ofs_put_date(b + f->block_bytes - 40u, value);
            if (mask & 4u)
                ofs_put_date(b + f->block_bytes - 28u, value);
        }
        if (!r) {
            ofs_checksum(f, b, 5);
            r = ofs_write_block(f, key, b);
        }
    }
    if (!r)
        r = ofs_cache_object(f, key);
    if (!r && n.parent == f->root)
        r = touch_directory(f, f->root);
    return ofs_end(f, r);
}
int ofs_set_metadata(OfsFs *f, const char *path, uint32_t operation, const void *value) {
    OfsNode n;
    int r = ofs_path(f, path, 0, &n);
    return r ? r : ofs_set_metadata_key(f, n.object, operation, value);
}
int ofs_set_label(OfsFs *f, const char *label) {
    uint8_t name[31];
    uint32_t length;
    int r = ofs_latin1(label, name, 30, &length, 0);
    if (!r && length)
        r = ofs_name_valid(name, length, 30);
    if (r)
        return r;
    r = ofs_begin(f);
    if (r)
        return r;
    uint8_t *b = f->scratch[1];
    r = ofs_metadata(f, f->root, b);
    if (!r) {
        r = ofs_put_strings(f, f->root, b, name, length, 0, 0);
        if (r)
            return ofs_end(f, r);
        ofs_checksum(f, b, 5);
        r = ofs_write_block(f, f->root, b);
    }
    return ofs_end(f, r);
}
