/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OFS_RELAYOUT_H
#define OFS_RELAYOUT_H
#include "../core/ofs_engine.h"
#include "infiltratr/fs/defrag_module.h"
typedef struct {
    const IfsDefragHostV2 *host;
    IfsDefragTarget *target;
} OfsDefragIo;
typedef struct { uint32_t old_key, new_key; } OfsKeyMove;
typedef struct {
    uint32_t key, bytes, count;
    uint32_t *old_data, *new_data;
} OfsLayoutFile;
typedef struct {
    OfsFs *fs;
    OfsDefragIo io;
    uint8_t *used, *source_used, *occupied, *source_data;
    size_t map_bytes;
    OfsLayoutFile *files;
    size_t file_count, file_capacity;
    OfsKeyMove *metadata;
    size_t metadata_count, metadata_capacity;
    uint32_t *directories;
    size_t directory_count, directory_capacity;
    uint64_t used_blocks, fragmented_files, fragmented_blocks;
    int planned, live;
} OfsLayout;
int ofs_layout_open(OfsLayout *, const IfsDefragHostV2 *, IfsDefragTarget *);
void ofs_layout_close(OfsLayout *);
int ofs_layout_plan(OfsLayout *, int);
int ofs_layout_write(OfsLayout *, IfsDefragTarget *);
int ofs_layout_verify(OfsLayout *, int);
int ofs_layout_hash(OfsLayout *, IfsDefragTarget *, char[65]);
int ofs_layout_source_hash(OfsLayout *, IfsDefragTarget *, char[65]);
int ofs_layout_commit(OfsLayout *, IfsDefragTarget *);
int ofs_layout_analyse(OfsLayout *);
int ofs_layout_token(const IfsDefragHostV2 *, IfsDefragTarget *, uint32_t, uint32_t,
                     uint32_t, uint32_t, uint32_t, char[65]);
#endif
