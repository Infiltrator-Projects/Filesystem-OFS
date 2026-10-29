/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_OFS_ENGINE_H
#define INFILTRATOR_OFS_ENGINE_H
#include "../module/ofs_v3.h"
#include "ofs_primitives.h"
#define OFS_MAX_BLOCK 32768u
#define OFS_CACHE_BYTES 65536u
#define OFS_MAX_NAME 107u
typedef struct {
    uint32_t key, object, parent, chain, link, original, bytes, protection;
    int32_t type, object_type;
    uint16_t uid, gid;
    uint8_t name[108], comment[80];
    uint32_t name_bytes, comment_bytes;
    IfsOfsDate changed, volume_changed, created;
} OfsNode;
typedef struct {
    uint32_t bucket, next, fast, visited;
} OfsDirCursor;
typedef struct {
    uint32_t owner, size, sequence, block, next, list, list_index;
} OfsFileCursor;
typedef struct {
    IfsOfsIoV3 io;
    uint32_t block_bytes, blocks, reserved, root, variant, hash_size;
    uint32_t bitmap_bits, bitmap_count, bitmap_valid, alloc_hint;
    uint32_t bitmap_ext, bitmap_ext_index;
    uint32_t *bitmap_keys, *system_keys;
    uint32_t system_count, used_blocks;
    uint32_t *cache_dirty;
    size_t cache_dirty_count, cache_dirty_capacity;
    int mounted, mutation, failed, preserve_volume_date;
    struct {
        uint64_t offset;
        uint32_t bytes;
        int valid;
        uint8_t data[OFS_CACHE_BYTES];
    } cache[2];
    /* Public operations own buffers 0..5. Bitmap/root helpers own 6; bitmap
     * updates and hard-link metadata resolution own 7, never concurrently. */
    uint8_t scratch[8][OFS_MAX_BLOCK];
    char link_path[2][OFS_MAX_BLOCK * 4u];
} OfsFs;
uint32_t ofs_be32(const void *);
void ofs_put32(void *, uint32_t);
uint16_t ofs_be16(const void *);
void ofs_put16(void *, uint16_t);
uint32_t ofs_word(const uint8_t *, uint32_t);
void ofs_setword(uint8_t *, uint32_t, uint32_t);
void ofs_zero(void *, size_t);
void ofs_copy(void *, const void *, size_t);
int ofs_equal(const void *, const void *, size_t);
int ofs_valid_block(const OfsFs *, uint32_t);
int ofs_read_block(OfsFs *, uint32_t, uint8_t *, int);
int ofs_write_block(OfsFs *, uint32_t, const uint8_t *);
int ofs_metadata(OfsFs *, uint32_t, uint8_t *);
void ofs_checksum(OfsFs *, uint8_t *, uint32_t);
void ofs_invalidate(OfsFs *);
int ofs_probe(OfsFs *, const IfsOfsIoV3 *, const IfsOfsMountV3 *);
void ofs_release(OfsFs *);
int ofs_format(OfsFs *, const IfsOfsIoV3 *, const IfsOfsFormatV3 *);
int ofs_begin(OfsFs *);
int ofs_end(OfsFs *, int);
int ofs_bitmap_key(OfsFs *, uint32_t, uint32_t *);
int ofs_index_bitmaps(OfsFs *, int);
int ofs_bitmap_free(OfsFs *, uint32_t, int *);
int ofs_bitmap_set(OfsFs *, uint32_t, int);
int ofs_allocate(OfsFs *, uint32_t *);
int ofs_space(OfsFs *, IfsOfsSpaceV3 *);
int ofs_clock(OfsFs *, IfsOfsDate *);
int ofs_date_valid(const IfsOfsDate *);
IfsOfsDate ofs_get_date(const uint8_t *);
void ofs_put_date(uint8_t *, const IfsOfsDate *);
int ofs_touch_root(OfsFs *, int);
int ofs_latin1(const char *, uint8_t *, uint32_t, uint32_t *, int);
int ofs_utf8(const uint8_t *, uint32_t, char *, uint32_t);
int ofs_name_valid(const uint8_t *, uint32_t, uint32_t);
uint32_t ofs_hash(const OfsFs *, const uint8_t *, uint32_t);
int ofs_name_equal(const OfsFs *, const uint8_t *, uint32_t, const uint8_t *, uint32_t);
int ofs_node(OfsFs *, uint32_t, OfsNode *, uint8_t *);
int ofs_directory(OfsFs *, uint32_t, uint8_t *);
int ofs_lookup(OfsFs *, uint32_t, const uint8_t *, uint32_t, OfsNode *);
int ofs_next(OfsFs *, uint32_t, OfsDirCursor *, OfsNode *);
int ofs_path(OfsFs *, const char *, int, OfsNode *);
int ofs_parent(OfsFs *, const char *, uint32_t *, uint8_t *, uint32_t *);
int ofs_entry(OfsFs *, const OfsNode *, IfsOfsEntryV3 *);
int ofs_pointer(OfsFs *, uint32_t, uint32_t, uint32_t, OfsFileCursor *, uint32_t *);
int ofs_read_file(OfsFs *, const OfsNode *, uint32_t, void *, uint32_t, OfsFileCursor *);
int ofs_check_file(OfsFs *, const OfsNode *);
int ofs_data(OfsFs *, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t *);
int ofs_resize(OfsFs *, OfsNode *, uint32_t);
int ofs_write_file(OfsFs *, OfsNode *, uint32_t, const void *, uint32_t);
int ofs_create(OfsFs *, const char *, int32_t, const char *, uint32_t);
int ofs_rename(OfsFs *, const char *, const char *);
int ofs_unlink(OfsFs *, const char *);
int ofs_set_metadata(OfsFs *, const char *, uint32_t, const void *);
int ofs_set_label(OfsFs *, const char *);
int ofs_readlink(OfsFs *, const char *, char *, uint32_t);
int ofs_verify(OfsFs *, IfsOfsCheckV3 *, int);
void ofs_sort_keys(uint32_t *, uint32_t);
int ofs_put_strings(OfsFs *, uint32_t, uint8_t *, const uint8_t *, uint32_t, const uint8_t *,
                    uint32_t);
int ofs_drop_comment(OfsFs *, uint32_t);
int ofs_cache_queue(OfsFs *, uint32_t);
int ofs_cache_object(OfsFs *, uint32_t);
int ofs_cache_commit(OfsFs *);
int ofs_cache_init(OfsFs *, uint32_t, uint32_t *);
int ofs_cache_drop(OfsFs *, uint32_t);
int ofs_cache_check(OfsFs *, uint32_t, int (*)(void *, uint32_t), void *);
#endif
