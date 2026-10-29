#include "../native/filesystems/ofs/core/ofs_primitives.h"
#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "OFS primitives test: %s\n", message);
    return 1;
}

int main(void)
{
    static const ifs_ofs_u8 normal[] = "ReadMe";
    static const ifs_ofs_u8 bad[] = "bad:name";
    ifs_ofs_u8 block[24] = { 0 };
    const ifs_ofs_u32 checksum = 0xfffffffaU;

    if (ifs_ofs_validate_name(normal, 6U, 1) != IFS_OFS_NAME_OK)
        return fail("valid name rejected");

    if (ifs_ofs_validate_name(bad, 8U, 1) !=
        IFS_OFS_NAME_INVALID_CHARACTER)
        return fail("colon-containing name accepted");

    if (ifs_ofs_fold_character((ifs_ofs_u8)'z', 0) !=
        (ifs_ofs_u8)'Z')
        return fail("ASCII case fold failed");

    if (ifs_ofs_fold_character(0xe4U, 1) != 0xc4U)
        return fail("international case fold failed");

    if (ifs_ofs_fold_character(0xf7U, 1) != 0xf7U)
        return fail("division sign was incorrectly folded");

    if (ifs_ofs_directory_hash(normal, 6U, 72U, 0) >= 72U)
        return fail("directory hash escaped hash table");

    {
        ifs_ofs_u32 root = 0U;
        if (ifs_ofs_root_block(2U, 2048U, &root) != 0 ||
            root != 1024U ||
            ifs_ofs_root_block(2U, 2049U, &root) != 0 ||
            root != 1025U ||
            ifs_ofs_root_block(2U, UINT32_MAX, &root) != 0 ||
            root != UINT32_C(0x80000000) ||
            ifs_ofs_root_block(2U, 2U, &root) == 0 ||
            ifs_ofs_root_block(2U, 2049U, NULL) == 0)
            return fail("canonical Amiga root block placement is wrong");
    }

    block[3] = 1U;
    block[7] = 2U;
    block[11] = 3U;
    if (ifs_ofs_block_checksum(block, sizeof(block)) != 6U)
        return fail("big-endian block checksum is wrong");

    if (ifs_ofs_checksum_word_value(block, sizeof(block), 5U) != checksum)
        return fail("checksum word calculation is wrong");


    {
        static const ifs_ofs_u8 volume_link[] = "Work:Tools//C";
        static const ifs_ofs_u8 relative_link[] = "dir//file";
        static const ifs_ofs_u8 prefix[] = "/";
        static const ifs_ofs_u8 unterminated[] = { 'a', 'b' };
        ifs_ofs_u8 translated[64] = { 0 };
        ifs_ofs_u32 translated_length = 0U;

        if (ifs_ofs_translate_symlink(
                volume_link, sizeof(volume_link), prefix, 1U,
                translated, sizeof(translated), &translated_length) !=
                IFS_OFS_SYMLINK_OK ||
            translated_length != 16U ||
            strcmp((const char *)translated, "/Work/Tools/../C") != 0)
            return fail("volume symlink translation is wrong");

        if (ifs_ofs_translate_symlink(
                relative_link, sizeof(relative_link), prefix, 1U,
                translated, sizeof(translated), &translated_length) !=
                IFS_OFS_SYMLINK_OK ||
            strcmp((const char *)translated, "dir/../file") != 0)
            return fail("relative parent symlink translation is wrong");

        if (ifs_ofs_translate_symlink(
                unterminated, sizeof(unterminated), prefix, 1U,
                translated, sizeof(translated), &translated_length) !=
                IFS_OFS_SYMLINK_SOURCE_UNTERMINATED)
            return fail("unterminated symlink source accepted");

        if (ifs_ofs_translate_symlink(
                volume_link, sizeof(volume_link), prefix, 1U,
                translated, 8U, &translated_length) !=
                IFS_OFS_SYMLINK_OUTPUT_TOO_SMALL)
            return fail("undersized symlink output accepted");

        {
            static const ifs_ofs_u8 unix_absolute[] =
                "/Tools/../C//gcc";
            static const ifs_ofs_u8 unix_relative[] =
                "src/./include/../main";
            static const ifs_ofs_u8 volume[] = "Work:";
            ifs_ofs_u8 encoded[64] = { 0 };
            ifs_ofs_u32 encoded_length = 0U;

            if (ifs_ofs_encode_symlink(
                    unix_absolute, sizeof(unix_absolute),
                    volume, 5U, encoded, sizeof(encoded),
                    &encoded_length) != IFS_OFS_SYMLINK_OK ||
                strcmp((const char *)encoded, "Work:Tools//C/gcc") != 0)
                return fail("absolute symlink encoding is wrong");

            if (ifs_ofs_encode_symlink(
                    unix_relative, sizeof(unix_relative),
                    volume, 5U, encoded, sizeof(encoded),
                    &encoded_length) != IFS_OFS_SYMLINK_OK ||
                strcmp((const char *)encoded, "src/include//main") != 0)
                return fail("relative symlink encoding is wrong");
        }
    }

    {
        ifs_ofs_u32 bits = 0U;
        ifs_ofs_u32 count = 0U;

        if (ifs_ofs_data_block_valid(1U, 2U, 100U) != 0)
            return fail("reserved block accepted as data");
        if (ifs_ofs_data_block_valid(2U, 2U, 100U) == 0)
            return fail("first data block rejected");
        if (ifs_ofs_data_block_valid(100U, 2U, 100U) != 0)
            return fail("one-past-end block accepted");
        if (ifs_ofs_bitmap_geometry(
                512U, 2U, 10000U, &bits, &count) != 0 ||
            bits != 4064U || count != 3U)
            return fail("AmigaDOS bitmap geometry is wrong");
        /* Physical DOS0 keys with bit 31 set are *unsigned* metadata,
         * not negative block offsets. Exercise the largest partition and
         * bitmap address ranges without allocating a terabyte image. */
        if (!ifs_ofs_data_block_valid(UINT32_C(0x80000000),
                                      2U, UINT32_MAX) ||
            !ifs_ofs_data_block_valid(UINT32_MAX - 1U,
                                      2U, UINT32_MAX) ||
            ifs_ofs_data_block_valid(UINT32_MAX, 2U, UINT32_MAX))
            return fail("unsigned DOS0 high block key boundary");
        if (ifs_ofs_bitmap_geometry(512U, 2U, UINT32_MAX,
                                    &bits, &count) != 0 ||
            bits != 4064U || count != 1056833U)
            return fail("unsigned DOS0 high bitmap geometry");

        {
            ifs_ofs_u32 bitmap_index = 0U, bit_index = 0U;
            const ifs_ofs_u32 high = UINT32_MAX - 1U;
            const ifs_ofs_u32 rel = high - 2U;
            if (ifs_ofs_bitmap_location(high, 2U, UINT32_MAX, 4064U,
                                        &bitmap_index, &bit_index) != 0 ||
                bitmap_index != rel / 4064U ||
                bit_index != rel % 4064U)
                return fail("unsigned DOS0 bitmap high-key mapping");
        }

        if (ifs_ofs_bitmap_bit_mask(31U) != 0x80000000U)
            return fail("AmigaDOS bitmap bit mask is wrong");
        if (ifs_ofs_bitmap_scan_mask(4U) != 0xfffffff0U)
            return fail("AmigaDOS bitmap scan mask is wrong");

        {
            ifs_ofs_u32 bitmap_index = 0U;
            ifs_ofs_u32 bit_index = 0U;
            ifs_ofs_u32 first = 0U;
            ifs_ofs_u32 run_mask = 0U;
            ifs_ofs_u32 run_length = 0U;

            if (ifs_ofs_bitmap_location(
                    4098U, 2U, 9000U, 4064U,
                    &bitmap_index, &bit_index) != 0 ||
                bitmap_index != 1U || bit_index != 32U)
                return fail("bitmap location calculation is wrong");

            if (ifs_ofs_bitmap_location(
                    1U, 2U, 9000U, 4064U,
                    &bitmap_index, &bit_index) == 0)
                return fail("reserved block accepted by bitmap locator");

            if (ifs_ofs_bitmap_valid_word_mask(0U) != 0U ||
                ifs_ofs_bitmap_valid_word_mask(5U) != 0x1fU ||
                ifs_ofs_bitmap_valid_word_mask(32U) != 0xffffffffU)
                return fail("bitmap valid-word mask is wrong");

        {
            ifs_ofs_u32 extension = 0U;
            ifs_ofs_u32 entry = 0U;
            ifs_ofs_u32 blocks = 0U;
            ifs_ofs_u32 extensions = 0U;

            if (ifs_ofs_file_block_location(
                    145U, 72U, &extension, &entry) != 0 ||
                extension != 2U || entry != 1U)
                return fail("file block location is wrong");

            if (ifs_ofs_file_block_count(
                    1025U, 512U, &blocks) != 0 || blocks != 3U)
                return fail("file block count is wrong");

            if (ifs_ofs_file_block_count(
                    0U, 512U, &blocks) != 0 || blocks != 0U)
                return fail("empty file block count is wrong");

            if (ifs_ofs_file_extension_count(
                    145U, 72U, &extensions) != 0 ||
                extensions != 3U)
                return fail("file extension count is wrong");

            if (ifs_ofs_file_extension_count(
                    0U, 72U, &extensions) != 0 ||
                extensions != 1U)
                return fail("empty file extension count is wrong");

            if (ifs_ofs_file_block_location(
                    0U, 0U, &extension, &entry) == 0 ||
                ifs_ofs_file_block_count(
                    1U, 0U, &blocks) == 0)
                return fail("invalid file geometry accepted");
        }

            if (ifs_ofs_bitmap_select_free_run(
                    0x000000f4U, 1U, 8U,
                    &first, &run_mask, &run_length) != 0 ||
                first != 2U || run_mask != 0x00000004U ||
                run_length != 1U)
                return fail("bitmap free-run selection is wrong");

            if (ifs_ofs_bitmap_select_free_run(
                    0x000000f0U, 0U, 8U,
                    &first, &run_mask, &run_length) != 0 ||
                first != 4U || run_mask != 0x000000f0U ||
                run_length != 4U)
                return fail("bitmap contiguous run selection is wrong");

            if (ifs_ofs_bitmap_select_free_run(
                    0xffffffffU, 0U, 0U,
                    &first, &run_mask, &run_length) == 0)
                return fail("zero-width bitmap word accepted");
        }
    }

    return 0;
}
