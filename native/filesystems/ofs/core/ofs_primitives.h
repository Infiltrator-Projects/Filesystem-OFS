#ifndef INFILTRATOR_OFS_PRIMITIVES_H
#define INFILTRATOR_OFS_PRIMITIVES_H

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u8 ifs_ofs_u8;
typedef u32 ifs_ofs_u32;
typedef u64 ifs_ofs_u64;
#else
#include <stdint.h>
typedef uint8_t ifs_ofs_u8;
typedef uint32_t ifs_ofs_u32;
typedef uint64_t ifs_ofs_u64;
#endif

/* Amiga root key: floor((reserved + total_blocks - 1) / 2).
 * Use a wide intermediate to cover unsigned 32-bit partition geometry. */
int ifs_ofs_root_block(ifs_ofs_u32 reserved_blocks,
                       ifs_ofs_u32 partition_blocks,
                       ifs_ofs_u32 *root_block);

int ifs_ofs_data_block_valid(
    ifs_ofs_u32 block,
    ifs_ofs_u32 reserved_blocks,
    ifs_ofs_u32 partition_blocks);

int ifs_ofs_bitmap_geometry(
    ifs_ofs_u32 block_size,
    ifs_ofs_u32 reserved_blocks,
    ifs_ofs_u32 partition_blocks,
    ifs_ofs_u32 *bits_per_bitmap,
    ifs_ofs_u32 *bitmap_count);

ifs_ofs_u32 ifs_ofs_bitmap_bit_mask(ifs_ofs_u32 bit_offset);
ifs_ofs_u32 ifs_ofs_bitmap_scan_mask(ifs_ofs_u32 bit_offset);

int ifs_ofs_bitmap_location(
    ifs_ofs_u32 block,
    ifs_ofs_u32 reserved_blocks,
    ifs_ofs_u32 partition_blocks,
    ifs_ofs_u32 bits_per_bitmap,
    ifs_ofs_u32 *bitmap_index,
    ifs_ofs_u32 *bit_index);

ifs_ofs_u32 ifs_ofs_bitmap_valid_word_mask(ifs_ofs_u32 valid_bits);

int ifs_ofs_bitmap_select_free_run(
    ifs_ofs_u32 word,
    ifs_ofs_u32 start_bit,
    ifs_ofs_u32 valid_bits,
    ifs_ofs_u32 *first_bit,
    ifs_ofs_u32 *run_mask,
    ifs_ofs_u32 *run_length);

int ifs_ofs_file_block_location(
    ifs_ofs_u32 logical_block,
    ifs_ofs_u32 entries_per_extension,
    ifs_ofs_u32 *extension_index,
    ifs_ofs_u32 *entry_index);

int ifs_ofs_file_block_count(
    ifs_ofs_u32 file_size,
    ifs_ofs_u32 data_bytes_per_block,
    ifs_ofs_u32 *block_count);

int ifs_ofs_file_extension_count(
    ifs_ofs_u32 block_count,
    ifs_ofs_u32 entries_per_extension,
    ifs_ofs_u32 *extension_count);

#define IFS_OFS_DOS_NAME_MAX 30U

typedef enum IfsOfsNameStatus {
    IFS_OFS_NAME_OK = 0,
    IFS_OFS_NAME_TOO_LONG,
    IFS_OFS_NAME_INVALID_CHARACTER
} IfsOfsNameStatus;

IfsOfsNameStatus ifs_ofs_validate_name(
    const ifs_ofs_u8 *name,
    ifs_ofs_u32 length,
    int no_truncate);

ifs_ofs_u8 ifs_ofs_fold_character(
    ifs_ofs_u8 character,
    int international_mode);

ifs_ofs_u32 ifs_ofs_directory_hash(
    const ifs_ofs_u8 *name,
    ifs_ofs_u32 length,
    ifs_ofs_u32 hash_table_size,
    int international_mode);

ifs_ofs_u32 ifs_ofs_block_checksum(
    const ifs_ofs_u8 *block,
    ifs_ofs_u32 block_size);

ifs_ofs_u32 ifs_ofs_checksum_word_value(
    const ifs_ofs_u8 *block,
    ifs_ofs_u32 block_size,
    ifs_ofs_u32 checksum_word_index);

typedef enum IfsOfsSymlinkStatus {
    IFS_OFS_SYMLINK_OK = 0,
    IFS_OFS_SYMLINK_INVALID_ARGUMENT,
    IFS_OFS_SYMLINK_SOURCE_UNTERMINATED,
    IFS_OFS_SYMLINK_OUTPUT_TOO_SMALL
} IfsOfsSymlinkStatus;

IfsOfsSymlinkStatus ifs_ofs_translate_symlink(
    const ifs_ofs_u8 *source,
    ifs_ofs_u32 source_capacity,
    const ifs_ofs_u8 *volume_prefix,
    ifs_ofs_u32 volume_prefix_length,
    ifs_ofs_u8 *output,
    ifs_ofs_u32 output_capacity,
    ifs_ofs_u32 *output_length);

IfsOfsSymlinkStatus ifs_ofs_encode_symlink(
    const ifs_ofs_u8 *source,
    ifs_ofs_u32 source_capacity,
    const ifs_ofs_u8 *volume_name,
    ifs_ofs_u32 volume_name_length,
    ifs_ofs_u8 *output,
    ifs_ofs_u32 output_capacity,
    ifs_ofs_u32 *output_length);

#endif
