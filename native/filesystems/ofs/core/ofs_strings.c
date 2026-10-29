/* SPDX-License-Identifier: GPL-3.0-or-later
 * Classic and LNFS metadata writers, OFS_SPECIFICATION section 13A.
 * The 112-byte NaC area and T_COMMENT are distinct from classic fields.
 */
#include "ofs_engine.h"

int ofs_put_strings(OfsFs *f, uint32_t key, uint8_t *b, const uint8_t *name, uint32_t n,
                    const uint8_t *comment, uint32_t c) {
    if (c > 79u || ofs_name_valid(name, n, key == f->root || f->variant != 6u ? 30u : 106u))
        return IFS_OFS_INVALID;
    uint32_t size = f->block_bytes;
    if (key == f->root || f->variant != 6u) {
        ofs_zero(b + size - 80u, 32u);
        b[size - 80u] = (uint8_t)n;
        ofs_copy(b + size - 79u, name, n);
        if (key != f->root) {
            ofs_zero(b + size - 184u, 80u);
            b[size - 184u] = (uint8_t)c;
            ofs_copy(b + size - 183u, comment, c);
        }
        return 0;
    }
    uint32_t old = ofs_be32(b + size - 72u), extra = old;
    int overflow = n + c + 2u > 112u, r;
    if (overflow) {
        if (!extra) {
            r = ofs_allocate(f, &extra);
            if (r)
                return r;
        }
        uint8_t *data = f->scratch[4];
        ofs_zero(data, size);
        ofs_setword(data, 0, 64);
        ofs_setword(data, 1, extra);
        ofs_setword(data, 2, key);
        data[24] = (uint8_t)c;
        ofs_copy(data + 25, comment, c);
        ofs_checksum(f, data, 5);
        r = ofs_write_block(f, extra, data);
        if (r)
            return r;
    }
    ofs_zero(b + size - 184u, 112u);
    b[size - 184u] = (uint8_t)n;
    ofs_copy(b + size - 183u, name, n);
    if (!overflow) {
        b[size - 183u + n] = (uint8_t)c;
        ofs_copy(b + size - 182u + n, comment, c);
    }
    ofs_put32(b + size - 72u, overflow ? extra : 0);
    if (!overflow && old)
        return ofs_bitmap_set(f, old, 1);
    return 0;
}
int ofs_drop_comment(OfsFs *f, uint32_t key) {
    if (f->variant != 6u || key == f->root)
        return 0;
    uint8_t *b = f->scratch[3];
    int r = ofs_metadata(f, key, b);
    if (r)
        return r;
    uint32_t comment = ofs_be32(b + f->block_bytes - 72u);
    return comment ? ofs_bitmap_set(f, comment, 1) : 0;
}
