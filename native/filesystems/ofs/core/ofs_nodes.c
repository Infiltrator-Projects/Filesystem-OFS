/* SPDX-License-Identifier: GPL-3.0-or-later
 * Portable namespace/metadata reads, adapted from the OFS Linux namespace
 * adapter and its canonical primitives. OFS_SPECIFICATION sections 8, 11..13A.
 * Public paths use UTF-8 and '/' with '.'/'..'. On-media names remain Latin-1;
 * native Amiga soft-link targets retain their own colon/parent semantics.
 */
#include "ofs_engine.h"

int ofs_name_valid(const uint8_t *name, uint32_t n, uint32_t limit) {
    if (!name || !n || n > limit)
        return IFS_OFS_INVALID;
    for (uint32_t i = 0; i < n; ++i)
        if (name[i] < 32u || name[i] == ':' || name[i] == '/' || (name[i] > 126u && name[i] < 160u))
            return IFS_OFS_INVALID;
    return 0;
}
int ofs_latin1(const char *s, uint8_t *out, uint32_t limit, uint32_t *length, int name) {
    if (!s || !out || !length)
        return IFS_OFS_INVALID;
    uint32_t n = 0;
    while (*s) {
        uint8_t c = (uint8_t)*s++;
        if (c >= 128u) {
            if (c < 0xc2u || c > 0xc3u)
                return IFS_OFS_INVALID;
            uint8_t d = (uint8_t)*s++;
            if ((d & 0xc0u) != 0x80u)
                return IFS_OFS_INVALID;
            c = (uint8_t)(((c & 3u) << 6) | (d & 63u));
        }
        if (n >= limit)
            return IFS_OFS_INVALID;
        out[n++] = c;
    }
    if (name && ofs_name_valid(out, n, limit))
        return IFS_OFS_INVALID;
    *length = n;
    return 0;
}
int ofs_utf8(const uint8_t *s, uint32_t n, char *out, uint32_t cap) {
    uint32_t at = 0;
    if (!out || !cap)
        return IFS_OFS_INVALID;
    for (uint32_t i = 0; i < n; ++i) {
        if (at + (s[i] >= 128u ? 2u : 1u) >= cap)
            return IFS_OFS_INVALID;
        if (s[i] >= 128u) {
            out[at++] = (char)(0xc0u | (s[i] >> 6));
            out[at++] = (char)(0x80u | (s[i] & 63u));
        } else
            out[at++] = (char)s[i];
    }
    out[at] = 0;
    return 0;
}
uint32_t ofs_hash(const OfsFs *f, const uint8_t *name, uint32_t n) {
    uint32_t h = n;
    for (uint32_t i = 0; i < n; ++i)
        h = (h * 13u + ifs_ofs_fold_character(name[i], f->variant != 0u)) & 0x7ffu;
    return h % f->hash_size;
}
int ofs_name_equal(const OfsFs *f, const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn) {
    if (an != bn)
        return 0;
    for (uint32_t i = 0; i < an; ++i)
        if (ifs_ofs_fold_character(a[i], f->variant != 0u) !=
            ifs_ofs_fold_character(b[i], f->variant != 0u))
            return 0;
    return 1;
}
static int node_strings(OfsFs *f, uint32_t key, const uint8_t *b, OfsNode *n) {
    uint32_t size = f->block_bytes;
    if (key == f->root || f->variant != 6u) {
        n->name_bytes = b[size - 80u];
        uint32_t limit = key == f->root ? 30u : 30u;
        if (n->name_bytes > limit ||
            (key != f->root && ofs_name_valid(b + size - 79u, n->name_bytes, limit)))
            return IFS_OFS_CORRUPT;
        ofs_copy(n->name, b + size - 79u, n->name_bytes);
        if (key != f->root) {
            n->comment_bytes = b[size - 184u];
            if (n->comment_bytes > 79u)
                return IFS_OFS_CORRUPT;
            ofs_copy(n->comment, b + size - 183u, n->comment_bytes);
        }
    } else {
        uint32_t at = size - 184u;
        n->name_bytes = b[at];
        if (ofs_name_valid(b + at + 1u, n->name_bytes, 107u))
            return IFS_OFS_CORRUPT;
        ofs_copy(n->name, b + at + 1u, n->name_bytes);
        uint32_t comment_key = ofs_be32(b + size - 72u);
        if (comment_key) {
            uint8_t *c = f->scratch[7];
            int r = ofs_read_block(f, comment_key, c, 0);
            if (r)
                return r;
            if (ofs_word(c, 0) != 64u || ofs_word(c, 1) != comment_key || ofs_word(c, 2) != key ||
                ifs_ofs_block_checksum(c, size) || c[24] > 79u)
                return IFS_OFS_CORRUPT;
            n->comment_bytes = c[24];
            ofs_copy(n->comment, c + 25, n->comment_bytes);
        } else {
            uint32_t offset = 1u + n->name_bytes;
            n->comment_bytes = b[at + offset];
            if (n->comment_bytes > 79u || offset + 1u + n->comment_bytes > 112u)
                return IFS_OFS_CORRUPT;
            ofs_copy(n->comment, b + at + offset + 1u, n->comment_bytes);
        }
    }
    n->name[n->name_bytes] = 0;
    n->comment[n->comment_bytes] = 0;
    return 0;
}
int ofs_node(OfsFs *f, uint32_t key, OfsNode *n, uint8_t *b) {
    if (!n || !b)
        return IFS_OFS_INVALID;
    int r = ofs_metadata(f, key, b);
    if (r)
        return r;
    ofs_zero(n, sizeof(*n));
    n->key = n->object = key;
    n->type = n->object_type = (int32_t)ofs_be32(b + f->block_bytes - 4u);
    if (key == f->root) {
        if (n->type != IFS_OFS_ROOT)
            return IFS_OFS_CORRUPT;
        n->changed = ofs_get_date(b + f->block_bytes - 92u);
        n->volume_changed = ofs_get_date(b + f->block_bytes - 40u);
        n->created = ofs_get_date(b + f->block_bytes - 28u);
    } else {
        if (n->type != IFS_OFS_FILE && n->type != IFS_OFS_DIRECTORY &&
            n->type != IFS_OFS_SOFTLINK && n->type != IFS_OFS_LINKFILE &&
            n->type != IFS_OFS_LINKDIR)
            return IFS_OFS_CORRUPT;
        n->parent = ofs_be32(b + f->block_bytes - 12u);
        n->chain = ofs_be32(b + f->block_bytes - 16u);
        n->original = ofs_be32(b + f->block_bytes - 44u);
        n->link = ofs_be32(b + f->block_bytes - 40u);
        if (!ofs_valid_block(f, n->parent) || (n->chain && !ofs_valid_block(f, n->chain)) ||
            (n->link && !ofs_valid_block(f, n->link)))
            return IFS_OFS_CORRUPT;
        n->bytes = n->type == IFS_OFS_FILE ? ofs_be32(b + f->block_bytes - 188u) : 0u;
        n->protection = ofs_be32(b + f->block_bytes - 192u);
        n->uid = ofs_be16(b + f->block_bytes - 196u);
        n->gid = ofs_be16(b + f->block_bytes - 194u);
        n->changed = ofs_get_date(b + f->block_bytes - (f->variant == 6u ? 60u : 92u));
    }
    r = node_strings(f, key, b, n);
    if (r)
        return r;
    if (n->type == IFS_OFS_LINKFILE || n->type == IFS_OFS_LINKDIR) {
        uint8_t *target = f->scratch[7];
        r = ofs_metadata(f, n->original, target);
        if (r)
            return r;
        int32_t type = (int32_t)ofs_be32(target + f->block_bytes - 4u);
        if (type != (n->type == IFS_OFS_LINKFILE ? IFS_OFS_FILE : IFS_OFS_DIRECTORY))
            return IFS_OFS_CORRUPT;
        n->object = n->original;
        n->object_type = type;
        n->bytes = type == IFS_OFS_FILE ? ofs_be32(target + f->block_bytes - 188u) : 0;
        n->protection = ofs_be32(target + f->block_bytes - 192u);
        n->uid = ofs_be16(target + f->block_bytes - 196u);
        n->gid = ofs_be16(target + f->block_bytes - 194u);
        n->changed = ofs_get_date(target + f->block_bytes - (f->variant == 6u ? 60u : 92u));
    }
    return 0;
}
int ofs_directory(OfsFs *f, uint32_t key, uint8_t *b) {
    int r = ofs_metadata(f, key, b);
    if (r)
        return r;
    int32_t type = (int32_t)ofs_be32(b + f->block_bytes - 4u);
    if (key == f->root)
        return type == IFS_OFS_ROOT && ofs_word(b, 3) == f->hash_size ? 0 : IFS_OFS_CORRUPT;
    return type == IFS_OFS_DIRECTORY && ofs_word(b, 2) == 0 && ofs_word(b, 3) == 0 &&
                   ofs_word(b, 4) == 0
               ? 0
               : IFS_OFS_CORRUPT;
}
static int chain_node(OfsFs *f, uint32_t key, uint32_t parent, uint32_t bucket, OfsNode *n,
                      uint8_t *b) {
    int r = ofs_node(f, key, n, b);
    if (r)
        return r;
    return n->parent == parent && ofs_hash(f, n->name, n->name_bytes) == bucket ? 0
                                                                                : IFS_OFS_CORRUPT;
}
static int advance_fast(OfsFs *f, uint32_t parent, uint32_t bucket, uint32_t *key) {
    OfsNode n;
    for (unsigned i = 0; i < 2 && *key; ++i) {
        int r = chain_node(f, *key, parent, bucket, &n, f->scratch[2]);
        if (r)
            return r;
        *key = n.chain;
    }
    return 0;
}
int ofs_lookup(OfsFs *f, uint32_t parent, const uint8_t *name, uint32_t length, OfsNode *out) {
    uint8_t *b = f->scratch[0];
    uint32_t bucket = ofs_hash(f, name, length);
    int r = ofs_directory(f, parent, b);
    if (r)
        return r;
    uint32_t key = ofs_word(b, 6u + bucket), fast = key, seen = 0;
    while (key) {
        if (++seen > f->blocks - f->reserved)
            return IFS_OFS_CORRUPT;
        r = chain_node(f, key, parent, bucket, out, f->scratch[1]);
        if (r)
            return r;
        if (ofs_name_equal(f, out->name, out->name_bytes, name, length))
            return 0;
        key = out->chain;
        r = advance_fast(f, parent, bucket, &fast);
        if (r)
            return r;
        if (key && key == fast)
            return IFS_OFS_CORRUPT;
    }
    return IFS_OFS_NOT_FOUND;
}
int ofs_next(OfsFs *f, uint32_t parent, OfsDirCursor *c, OfsNode *out) {
    if (!c || !out || c->bucket > f->hash_size)
        return IFS_OFS_INVALID;
    uint8_t *b = f->scratch[0];
    int r = ofs_directory(f, parent, b);
    if (r)
        return r;
    while (c->bucket < f->hash_size) {
        if (!c->next) {
            c->next = ofs_word(b, 6u + c->bucket);
            c->fast = c->next;
        }
        if (!c->next) {
            ++c->bucket;
            continue;
        }
        if (++c->visited > f->blocks - f->reserved)
            return IFS_OFS_CORRUPT;
        r = chain_node(f, c->next, parent, c->bucket, out, f->scratch[1]);
        if (r)
            return r;
        c->next = out->chain;
        r = advance_fast(f, parent, c->bucket, &c->fast);
        if (r)
            return r;
        if (c->next && c->next == c->fast)
            return IFS_OFS_CORRUPT;
        if (!c->next) {
            ++c->bucket;
            c->fast = 0;
        }
        return 0;
    }
    return IFS_OFS_END;
}
int ofs_entry(OfsFs *f, const OfsNode *n, IfsOfsEntryV3 *out) {
    (void)f;
    if (!out || !n)
        return IFS_OFS_INVALID;
    ofs_zero(out, sizeof(*out));
    out->structure_bytes = sizeof(*out);
    out->block = n->key;
    out->original = n->object;
    out->bytes = n->bytes;
    out->type = n->type;
    out->protection = n->protection;
    out->uid = n->uid;
    out->gid = n->gid;
    out->changed = n->changed;
    out->created = n->created;
    out->volume_changed = n->volume_changed;
    int r = ofs_utf8(n->name, n->name_bytes, out->name, sizeof(out->name));
    return r ? r : ofs_utf8(n->comment, n->comment_bytes, out->comment, sizeof(out->comment));
}
static int component(const char **text, uint8_t name[108], uint32_t *length) {
    char encoded[217];
    uint32_t n = 0;
    const char *p = *text;
    while (*p && *p != '/') {
        if (n + 1u >= sizeof(encoded))
            return IFS_OFS_INVALID;
        encoded[n++] = *p++;
    }
    encoded[n] = 0;
    *text = p;
    return ofs_latin1(encoded, name, 107u, length, 1);
}
static int resolve_path(OfsFs *f, const char *path, uint32_t start, unsigned depth, int follow,
                        OfsNode *out);
static int resolve_link(OfsFs *f, const OfsNode *node, const char *remaining, unsigned depth,
                        int follow, OfsNode *out) {
    if (depth >= 32u)
        return IFS_OFS_CORRUPT;
    uint8_t *b = f->scratch[3];
    int r = ofs_metadata(f, node->key, b);
    if (r)
        return r;
    uint32_t cap = f->block_bytes - 224u, n = 0;
    while (n < cap && b[24u + n])
        ++n;
    if (n == cap)
        return IFS_OFS_CORRUPT;
    char *target = f->link_path[depth & 1u];
    uint32_t used = 0, start = node->parent;
    uint32_t colon = n;
    for (uint32_t i = 0; i < n; ++i)
        if (b[24u + i] == ':') {
            colon = i;
            break;
        }
    uint32_t at = 0;
    if (colon < n) {
        OfsNode root;
        r = ofs_node(f, f->root, &root, f->scratch[4]);
        if (r)
            return r;
        if (colon && !ofs_name_equal(f, b + 24, colon, root.name, root.name_bytes))
            return IFS_OFS_NOT_FOUND;
        start = f->root;
        at = colon + 1u;
    }
    /* In a native Amiga target a leading slash, or an empty component after
     * a separator, denotes a parent. Public POSIX paths keep ordinary '/'. */
    int separator = 1;
    for (; at < n; ++at) {
        uint8_t ch = b[24u + at];
        if (ch == '/' && separator) {
            if (used + 3u >= OFS_MAX_BLOCK * 4u)
                return IFS_OFS_INVALID;
            target[used++] = '.';
            target[used++] = '.';
            target[used++] = '/';
        } else {
            if (used + 2u >= OFS_MAX_BLOCK * 4u)
                return IFS_OFS_INVALID;
            if (ch >= 128u) {
                target[used++] = (char)(0xc0u | (ch >> 6));
                target[used++] = (char)(0x80u | (ch & 63u));
            } else
                target[used++] = (char)ch;
        }
        separator = ch == '/';
    }
    while (*remaining == '/')
        ++remaining;
    if (*remaining && used && target[used - 1u] != '/')
        target[used++] = '/';
    while (*remaining) {
        if (used + 1u >= OFS_MAX_BLOCK * 4u)
            return IFS_OFS_INVALID;
        target[used++] = *remaining++;
    }
    target[used] = 0;
    return resolve_path(f, target, start, depth + 1u, follow, out);
}
static int resolve_path(OfsFs *f, const char *path, uint32_t start, unsigned depth, int follow,
                        OfsNode *out) {
    if (!path || !out || depth > 32u)
        return IFS_OFS_INVALID;
    uint32_t directory = start;
    const char *p = path;
    if (*p == '/') {
        directory = f->root;
        while (*p == '/')
            ++p;
    }
    while (*p) {
        uint8_t name[108];
        uint32_t length;
        int r = component(&p, name, &length);
        if (r)
            return r;
        const char *rest = p;
        while (*rest == '/')
            ++rest;
        if (length == 1u && name[0] == '.') {
            p = rest;
            continue;
        }
        if (length == 2u && name[0] == '.' && name[1] == '.') {
            OfsNode n;
            r = ofs_node(f, directory, &n, f->scratch[1]);
            if (r)
                return r;
            directory = directory == f->root ? f->root : n.parent;
            p = rest;
            continue;
        }
        r = ofs_lookup(f, directory, name, length, out);
        if (r)
            return r;
        if (out->type == IFS_OFS_SOFTLINK && (*rest || follow))
            return resolve_link(f, out, rest, depth, follow, out);
        if (!*rest)
            return 0;
        if (out->object_type != IFS_OFS_DIRECTORY && out->object_type != IFS_OFS_ROOT)
            return IFS_OFS_NOT_FOUND;
        directory = out->object;
        p = rest;
    }
    return ofs_node(f, directory, out, f->scratch[1]);
}
int ofs_path(OfsFs *f, const char *path, int follow, OfsNode *out) {
    if (!f->mounted)
        return IFS_OFS_INVALID;
    return resolve_path(f, path, f->root, 0, follow, out);
}
int ofs_parent(OfsFs *f, const char *path, uint32_t *parent, uint8_t *name, uint32_t *length) {
    if (!path || !parent || !name || !length)
        return IFS_OFS_INVALID;
    const char *last = path, *p = path;
    while (*p) {
        if (*p == '/')
            last = p + 1;
        ++p;
    }
    if (!*last)
        return IFS_OFS_INVALID;
    const char *end = last;
    int r = component(&end, name, length);
    if (r || *end)
        return IFS_OFS_INVALID;
    if ((*length == 1u && name[0] == '.') || (*length == 2u && name[0] == '.' && name[1] == '.'))
        return IFS_OFS_INVALID;
    if (*length > (f->variant == 6u ? 106u : 30u))
        return IFS_OFS_INVALID;
    if (last == path) {
        *parent = f->root;
        return 0;
    }
    size_t count = (size_t)(last - path);
    if (count >= OFS_MAX_BLOCK * 4u)
        return IFS_OFS_INVALID;
    /* Preserve the whole parent path; no classic 30/512-entry path cap. */
    char *text = f->link_path[1];
    ofs_copy(text, path, count);
    text[count] = 0;
    OfsNode dir;
    r = ofs_path(f, text, 1, &dir);
    if (r)
        return r;
    if (dir.object_type != IFS_OFS_DIRECTORY && dir.object_type != IFS_OFS_ROOT)
        return IFS_OFS_INVALID;
    *parent = dir.object;
    return 0;
}
int ofs_readlink(OfsFs *f, const char *path, char *out, uint32_t cap) {
    OfsNode n;
    int r = ofs_path(f, path, 0, &n);
    if (r)
        return r;
    if (n.type != IFS_OFS_SOFTLINK)
        return IFS_OFS_INVALID;
    uint8_t *b = f->scratch[3];
    r = ofs_metadata(f, n.key, b);
    if (r)
        return r;
    uint32_t bytes = 0, limit = f->block_bytes - 224u;
    while (bytes < limit && b[24u + bytes])
        ++bytes;
    return bytes == limit ? IFS_OFS_CORRUPT : ofs_utf8(b + 24, bytes, out, cap);
}
