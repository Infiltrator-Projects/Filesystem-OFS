#include "ofs_primitives.h"

static ifs_ofs_u32 ifs_ofs_read_be32(const ifs_ofs_u8 *data)
{
    return ((ifs_ofs_u32)data[0] << 24) |
           ((ifs_ofs_u32)data[1] << 16) |
           ((ifs_ofs_u32)data[2] << 8) |
           (ifs_ofs_u32)data[3];
}

IfsOfsNameStatus ifs_ofs_validate_name(
    const ifs_ofs_u8 *const name,
    const ifs_ofs_u32 length,
    const int no_truncate)
{
    ifs_ofs_u32 index;
    ifs_ofs_u32 checked_length = length;

    if (name == 0 && length != 0U)
        return IFS_OFS_NAME_INVALID_CHARACTER;

    if (checked_length > IFS_OFS_DOS_NAME_MAX) {
        if (no_truncate != 0)
            return IFS_OFS_NAME_TOO_LONG;
        checked_length = IFS_OFS_DOS_NAME_MAX;
    }

    for (index = 0U; index < checked_length; ++index) {
        const ifs_ofs_u8 ch = name[index];

        if (ch < 0x20U || ch == 0x3aU ||
            (ch > 0x7eU && ch < 0xa0U))
            return IFS_OFS_NAME_INVALID_CHARACTER;
    }

    return IFS_OFS_NAME_OK;
}

ifs_ofs_u8 ifs_ofs_fold_character(
    const ifs_ofs_u8 character,
    const int international_mode)
{
    if ((character >= (ifs_ofs_u8)'a' &&
         character <= (ifs_ofs_u8)'z') ||
        (international_mode != 0 &&
         character >= 0xe0U && character <= 0xfeU &&
         character != 0xf7U))
        return (ifs_ofs_u8)(character - 0x20U);

    return character;
}

ifs_ofs_u32 ifs_ofs_directory_hash(
    const ifs_ofs_u8 *const name,
    ifs_ofs_u32 length,
    const ifs_ofs_u32 hash_table_size,
    const int international_mode)
{
    ifs_ofs_u32 hash;
    ifs_ofs_u32 index;

    if (name == 0 || hash_table_size == 0U)
        return 0U;

    if (length > IFS_OFS_DOS_NAME_MAX)
        length = IFS_OFS_DOS_NAME_MAX;

    hash = length;
    for (index = 0U; index < length; ++index) {
        hash = (hash * 13U +
                ifs_ofs_fold_character(name[index], international_mode)) &
               0x7ffU;
    }

    return hash % hash_table_size;
}

ifs_ofs_u32 ifs_ofs_block_checksum(
    const ifs_ofs_u8 *const block,
    const ifs_ofs_u32 block_size)
{
    ifs_ofs_u32 sum = 0U;
    ifs_ofs_u32 offset;

    if (block == 0 || (block_size & 3U) != 0U)
        return 0U;

    for (offset = 0U; offset < block_size; offset += 4U)
        sum += ifs_ofs_read_be32(block + offset);

    return sum;
}

ifs_ofs_u32 ifs_ofs_checksum_word_value(
    const ifs_ofs_u8 *const block,
    const ifs_ofs_u32 block_size,
    const ifs_ofs_u32 checksum_word_index)
{
    ifs_ofs_u32 sum = 0U;
    ifs_ofs_u32 offset;
    const ifs_ofs_u32 checksum_offset = checksum_word_index * 4U;

    if (block == 0 || (block_size & 3U) != 0U ||
        checksum_offset > block_size - 4U)
        return 0U;

    for (offset = 0U; offset < block_size; offset += 4U) {
        if (offset != checksum_offset)
            sum += ifs_ofs_read_be32(block + offset);
    }

    return 0U - sum;
}

int ifs_ofs_root_block(
    const ifs_ofs_u32 reserved_blocks,
    const ifs_ofs_u32 partition_blocks,
    ifs_ofs_u32 *const root_block)
{
    ifs_ofs_u64 middle;

    if (!root_block || reserved_blocks >= partition_blocks)
        return -1;

    middle = ((ifs_ofs_u64)reserved_blocks +
              (ifs_ofs_u64)partition_blocks - 1U) / 2U;
    if (middle < reserved_blocks || middle >= partition_blocks)
        return -1;

    *root_block = (ifs_ofs_u32)middle;
    return 0;
}

int ifs_ofs_data_block_valid(
    const ifs_ofs_u32 block,
    const ifs_ofs_u32 reserved_blocks,
    const ifs_ofs_u32 partition_blocks)
{
    return reserved_blocks < partition_blocks &&
           block >= reserved_blocks &&
           block < partition_blocks;
}

int ifs_ofs_bitmap_geometry(
    const ifs_ofs_u32 block_size,
    const ifs_ofs_u32 reserved_blocks,
    const ifs_ofs_u32 partition_blocks,
    ifs_ofs_u32 *const bits_per_bitmap,
    ifs_ofs_u32 *const bitmap_count)
{
    ifs_ofs_u32 bits;
    ifs_ofs_u32 data_blocks;

    if (bits_per_bitmap == 0 || bitmap_count == 0 ||
        block_size < 8U ||
        block_size > 0x1fffffffU ||
        reserved_blocks >= partition_blocks)
        return -1;

    bits = block_size * 8U - 32U;
    if (bits == 0U)
        return -1;

    data_blocks = partition_blocks - reserved_blocks;
    *bits_per_bitmap = bits;
    *bitmap_count = data_blocks / bits +
        (data_blocks % bits != 0U ? 1U : 0U);
    return 0;
}

ifs_ofs_u32 ifs_ofs_bitmap_bit_mask(const ifs_ofs_u32 bit_offset)
{
    return 1U << (bit_offset & 31U);
}

ifs_ofs_u32 ifs_ofs_bitmap_scan_mask(const ifs_ofs_u32 bit_offset)
{
    return 0xffffffffU << (bit_offset & 31U);
}

int ifs_ofs_bitmap_location(
    const ifs_ofs_u32 block,
    const ifs_ofs_u32 reserved_blocks,
    const ifs_ofs_u32 partition_blocks,
    const ifs_ofs_u32 bits_per_bitmap,
    ifs_ofs_u32 *const bitmap_index,
    ifs_ofs_u32 *const bit_index)
{
    ifs_ofs_u32 relative;

    if (bitmap_index == 0 || bit_index == 0 || bits_per_bitmap == 0U ||
        !ifs_ofs_data_block_valid(
            block, reserved_blocks, partition_blocks))
        return -1;

    relative = block - reserved_blocks;
    *bitmap_index = relative / bits_per_bitmap;
    *bit_index = relative % bits_per_bitmap;
    return 0;
}

ifs_ofs_u32 ifs_ofs_bitmap_valid_word_mask(
    const ifs_ofs_u32 valid_bits)
{
    if (valid_bits == 0U)
        return 0U;
    if (valid_bits >= 32U)
        return 0xffffffffU;
    return (1U << valid_bits) - 1U;
}

int ifs_ofs_bitmap_select_free_run(
    const ifs_ofs_u32 word,
    const ifs_ofs_u32 start_bit,
    const ifs_ofs_u32 valid_bits,
    ifs_ofs_u32 *const first_bit,
    ifs_ofs_u32 *const run_mask,
    ifs_ofs_u32 *const run_length)
{
    ifs_ofs_u32 bit;
    ifs_ofs_u32 mask = 0U;
    ifs_ofs_u32 length = 0U;
    const ifs_ofs_u32 limit = valid_bits > 32U ? 32U : valid_bits;

    if (first_bit == 0 || run_mask == 0 || run_length == 0 ||
        start_bit >= limit)
        return -1;

    for (bit = start_bit; bit < limit; ++bit) {
        if ((word & (1U << bit)) != 0U)
            break;
    }
    if (bit == limit)
        return -1;

    *first_bit = bit;
    while (bit < limit && (word & (1U << bit)) != 0U) {
        mask |= 1U << bit;
        length++;
        bit++;
    }

    *run_mask = mask;
    *run_length = length;
    return 0;
}

int ifs_ofs_file_block_location(
    const ifs_ofs_u32 logical_block,
    const ifs_ofs_u32 entries_per_extension,
    ifs_ofs_u32 *const extension_index,
    ifs_ofs_u32 *const entry_index)
{
    if (entries_per_extension == 0U ||
        extension_index == 0 || entry_index == 0)
        return -1;

    *extension_index = logical_block / entries_per_extension;
    *entry_index = logical_block % entries_per_extension;
    return 0;
}

int ifs_ofs_file_block_count(
    const ifs_ofs_u32 file_size,
    const ifs_ofs_u32 data_bytes_per_block,
    ifs_ofs_u32 *const block_count)
{
    if (data_bytes_per_block == 0U || block_count == 0)
        return -1;

    if (file_size == 0U) {
        *block_count = 0U;
        return 0;
    }

    *block_count =
        (file_size - 1U) / data_bytes_per_block + 1U;
    return 0;
}

int ifs_ofs_file_extension_count(
    const ifs_ofs_u32 block_count,
    const ifs_ofs_u32 entries_per_extension,
    ifs_ofs_u32 *const extension_count)
{
    if (entries_per_extension == 0U || extension_count == 0)
        return -1;

    if (block_count == 0U) {
        *extension_count = 1U;
        return 0;
    }

    *extension_count =
        (block_count - 1U) / entries_per_extension + 1U;
    return 0;
}


IfsOfsSymlinkStatus ifs_ofs_translate_symlink(
    const ifs_ofs_u8 *const source,
    const ifs_ofs_u32 source_capacity,
    const ifs_ofs_u8 *const volume_prefix,
    const ifs_ofs_u32 volume_prefix_length,
    ifs_ofs_u8 *const output,
    const ifs_ofs_u32 output_capacity,
    ifs_ofs_u32 *const output_length)
{
    ifs_ofs_u32 source_length = 0U;
    ifs_ofs_u32 colon = source_capacity;
    ifs_ofs_u32 source_index = 0U;
    ifs_ofs_u32 output_index = 0U;
    ifs_ofs_u8 previous = 0U;
    ifs_ofs_u32 index;

    if (source == 0 || output == 0 || output_length == 0 ||
        output_capacity == 0U ||
        (volume_prefix == 0 && volume_prefix_length != 0U))
        return IFS_OFS_SYMLINK_INVALID_ARGUMENT;

    while (source_length < source_capacity &&
           source[source_length] != 0U) {
        if (source[source_length] == (ifs_ofs_u8)':' &&
            colon == source_capacity)
            colon = source_length;
        source_length++;
    }

    if (source_length == source_capacity) {
        output[0] = 0U;
        *output_length = 0U;
        return IFS_OFS_SYMLINK_SOURCE_UNTERMINATED;
    }

#define IFS_OFS_SYMLINK_APPEND(ch)                                      \
    do {                                                                  \
        if (output_index + 1U >= output_capacity) {                       \
            output[0] = 0U;                                               \
            *output_length = 0U;                                          \
            return IFS_OFS_SYMLINK_OUTPUT_TOO_SMALL;                    \
        }                                                                 \
        output[output_index++] = (ifs_ofs_u8)(ch);                      \
    } while (0)

    if (colon != source_capacity) {
        for (index = 0U; index < volume_prefix_length; ++index)
            IFS_OFS_SYMLINK_APPEND(volume_prefix[index]);

        for (index = 0U; index < colon; ++index)
            IFS_OFS_SYMLINK_APPEND(source[index]);

        IFS_OFS_SYMLINK_APPEND('/');
        previous = (ifs_ofs_u8)'/';
        source_index = colon + 1U;
    }

    for (; source_index < source_length; ++source_index) {
        const ifs_ofs_u8 character = source[source_index];

        if (character == (ifs_ofs_u8)'/' &&
            previous == (ifs_ofs_u8)'/') {
            IFS_OFS_SYMLINK_APPEND('.');
            IFS_OFS_SYMLINK_APPEND('.');
        }

        IFS_OFS_SYMLINK_APPEND(character);
        previous = character;
    }

#undef IFS_OFS_SYMLINK_APPEND

    output[output_index] = 0U;
    *output_length = output_index;
    return IFS_OFS_SYMLINK_OK;
}

IfsOfsSymlinkStatus ifs_ofs_encode_symlink(
    const ifs_ofs_u8 *const source,
    const ifs_ofs_u32 source_capacity,
    const ifs_ofs_u8 *const volume_name,
    const ifs_ofs_u32 volume_name_length,
    ifs_ofs_u8 *const output,
    const ifs_ofs_u32 output_capacity,
    ifs_ofs_u32 *const output_length)
{
    ifs_ofs_u32 source_length = 0U;
    ifs_ofs_u32 input = 0U;
    ifs_ofs_u32 output_index = 0U;
    ifs_ofs_u8 previous = (ifs_ofs_u8)'/';
    ifs_ofs_u32 index;

    if (source == 0 || output == 0 || output_length == 0 ||
        output_capacity == 0U ||
        (volume_name == 0 && volume_name_length != 0U))
        return IFS_OFS_SYMLINK_INVALID_ARGUMENT;

    while (source_length < source_capacity &&
           source[source_length] != 0U)
        source_length++;

    if (source_length == source_capacity) {
        output[0] = 0U;
        *output_length = 0U;
        return IFS_OFS_SYMLINK_SOURCE_UNTERMINATED;
    }

#define IFS_OFS_ENCODE_APPEND(ch)                                      \
    do {                                                                 \
        if (output_index + 1U >= output_capacity) {                      \
            output[0] = 0U;                                              \
            *output_length = 0U;                                         \
            return IFS_OFS_SYMLINK_OUTPUT_TOO_SMALL;                   \
        }                                                                \
        output[output_index++] = (ifs_ofs_u8)(ch);                     \
    } while (0)

    if (source_length != 0U && source[0] == (ifs_ofs_u8)'/') {
        while (input < source_length &&
               source[input] == (ifs_ofs_u8)'/')
            input++;

        for (index = 0U; index < volume_name_length; ++index)
            IFS_OFS_ENCODE_APPEND(volume_name[index]);
    }

    while (input < source_length) {
        const ifs_ofs_u8 character = source[input++];

        if (character == (ifs_ofs_u8)'.' &&
            previous == (ifs_ofs_u8)'/' &&
            input < source_length &&
            source[input] == (ifs_ofs_u8)'.' &&
            input + 1U < source_length &&
            source[input + 1U] == (ifs_ofs_u8)'/') {
            IFS_OFS_ENCODE_APPEND('/');
            input += 2U;
            previous = (ifs_ofs_u8)'/';
        } else if (character == (ifs_ofs_u8)'.' &&
                   previous == (ifs_ofs_u8)'/' &&
                   input < source_length &&
                   source[input] == (ifs_ofs_u8)'/') {
            input++;
            previous = (ifs_ofs_u8)'/';
        } else {
            IFS_OFS_ENCODE_APPEND(character);
            previous = character;
        }

        if (previous == (ifs_ofs_u8)'/') {
            while (input < source_length &&
                   source[input] == (ifs_ofs_u8)'/')
                input++;
        }
    }

#undef IFS_OFS_ENCODE_APPEND

    output[output_index] = 0U;
    *output_length = output_index;
    return IFS_OFS_SYMLINK_OK;
}
