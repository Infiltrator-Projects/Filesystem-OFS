/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../native/filesystems/ofs/efi/reader.h"
#include "../native/filesystems/ofs/core/ofs_primitives.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Assertions must execute even in Release / -DNDEBUG builds. */
#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "REQUIRE failed at %s:%d: %s\\n", \
                __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

enum { COUNT = 1024, BLOCK = 512 };
static uint32_t read_calls;
static uint8_t media[COUNT][BLOCK];
static void put(uint8_t *b, unsigned word, uint32_t n)
{
    b[word * 4] = (uint8_t)(n >> 24);
    b[word * 4 + 1] = (uint8_t)(n >> 16);
    b[word * 4 + 2] = (uint8_t)(n >> 8);
    b[word * 4 + 3] = (uint8_t)n;
}
static void checksum(uint8_t *b)
{
    put(b, 5, ifs_ofs_checksum_word_value(b, BLOCK, 5));
}
static int rd(void *unused, uint32_t block, uint8_t out[512])
{
    (void)unused;
    if (block >= COUNT)
        return -1;
    ++read_calls;
    memcpy(out, media[block], BLOCK);
    return 0;
}
static void label(uint8_t *b, const char *s)
{
    size_t n = strlen(s);
    REQUIRE(n <= 30U);
    b[432] = (uint8_t)n;
    memcpy(b + 433, s, n);
}
/* Synthetic unsigned-address test: never allocate a multi-terabyte image.
 * The callback models a valid root and directory above signed 31-bit block
 * addresses, and rejects every unrecognised sector. */
static const uint32_t LARGE_DIR = UINT32_C(0x80000020);
static int rd_large(void *opaque, uint32_t block, uint8_t out[512])
{
    const uint32_t blocks = *(const uint32_t *)opaque;
    uint32_t bucket;
    memset(out, 0, BLOCK);
    if (block == 0U) {
        put(out, 0, 0x444f5300U);
        return 0;
    }
    if (block == (uint32_t)(((uint64_t)blocks + 1U) / 2U)) {
        put(out, 0, 2U);
        put(out, 3, 72U);
        put(out, 127, 1U);
        label(out, "LARGE_OFS");
        bucket = ifs_ofs_directory_hash((const uint8_t *)"boot", 4U, 72U, 0);
        put(out, 6U + bucket, LARGE_DIR);
        checksum(out);
        return 0;
    }
    if (block == LARGE_DIR) {
        put(out, 0, 2U);
        put(out, 1, LARGE_DIR);
        put(out, 125, (uint32_t)(((uint64_t)blocks + 1U) / 2U));
        put(out, 127, 2U);
        label(out, "boot");
        checksum(out);
        return 0;
    }
    return -1;
}

static void fixture(void)
{
    uint8_t *root = media[COUNT / 2], *dir = media[4];
    uint8_t *file = media[5], *data = media[6];
    unsigned slot;
    memset(media, 0, sizeof(media));
    put(media[0], 0, 0x444f5300U);
    put(root, 0, 2);
    put(root, 3, 72);
    put(root, 127, 1);
    label(root, "OFSBOOT");
    slot = ifs_ofs_directory_hash((const uint8_t *)"boot", 4, 72, 0);
    put(root, 6 + slot, 4);
    checksum(root);
    put(dir, 0, 2);
    put(dir, 1, 4);
    put(dir, 125, COUNT / 2);
    put(dir, 127, 2);
    label(dir, "boot");
    slot = ifs_ofs_directory_hash((const uint8_t *)"kernel.efi", 10, 72, 0);
    put(dir, 6 + slot, 5);
    checksum(dir);
    put(file, 0, 2);
    put(file, 1, 5);
    put(file, 2, 1);
    put(file, 4, 6);
    put(file, 81, 5);
    put(file, 125, 4);
    put(file, 127, (uint32_t)-3);
    label(file, "kernel.efi");
    checksum(file);
    put(data, 0, 8);
    put(data, 1, 5);
    put(data, 2, 1);
    put(data, 3, 5);
    memcpy(data + 24, "HELLO", 5);
    checksum(data);
}
int main(void)
{
    IfsOfsEfiVolume v = {0};
    IfsOfsEfiEntry directory, file, child, root;
    int result;
    uint32_t entries;
    IfsOfsEfiDirCursor dcur = {0};
    IfsOfsEfiFileCursor fcur = {0};
    uint8_t name[31], buf[8] = {0};
    fixture();
    REQUIRE(ifs_ofs_efi_mount(&v, COUNT, rd, NULL) == 0);
    REQUIRE(ifs_ofs_efi_lookup(&v, &(IfsOfsEfiEntry){COUNT / 2, 0, IFS_OFS_EFI_DIRECTORY},
                              (const uint8_t *)"BOOT", 4, &directory) == 0);
    REQUIRE(ifs_ofs_efi_lookup(&v, &directory,
                              (const uint8_t *)"kernel.efi", 10, &file) == 0);
    REQUIRE(file.size == 5 && file.type == IFS_OFS_EFI_FILE);
    REQUIRE(ifs_ofs_efi_read(&v, &file, 0, buf, 5, &fcur) == 0);
    REQUIRE(memcmp(buf, "HELLO", 5) == 0);
    REQUIRE(ifs_ofs_efi_read(&v, &file, 2, buf, 3, &fcur) == 0);
    REQUIRE(memcmp(buf, "LLO", 3) == 0);
    REQUIRE(ifs_ofs_efi_read(&v, &file, 4, buf, 2, &fcur) == -1);
    REQUIRE(ifs_ofs_efi_next(&v, &directory, &dcur, &child, name) == 0);
    REQUIRE(strcmp((const char *)name, "kernel.efi") == 0);
    REQUIRE(ifs_ofs_efi_next(&v, &directory, &dcur, &child, name) == 1);
    REQUIRE(ifs_ofs_efi_lookup(&v, &directory,
                              (const uint8_t *)"missing", 7, &child) == 1);
    media[6][25] ^= 1;
    REQUIRE(ifs_ofs_efi_read(&v, &file, 0, buf, 5, NULL) == -1);
    fixture();
    put(media[5], 4, COUNT + 100U);
    checksum(media[5]);
    REQUIRE(ifs_ofs_efi_read(&v, &file, 0, buf, 5, NULL) == -1);
    /* A checksum-valid hash-chain cycle must fail within the EFI read
     * budget, regardless of the size of its containing partition. */
    fixture();
    put(media[4], 124, 4U);
    checksum(media[4]);
    root = ifs_ofs_efi_root(&v);
    read_calls = 0U;
    REQUIRE(ifs_ofs_efi_lookup(&v, &root,
                              (const uint8_t *)"boot100", 7, &child) == -1);
    REQUIRE(read_calls <= IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES + 2U);
    dcur = (IfsOfsEfiDirCursor){0};
    entries = 0U;
    read_calls = 0U;
    while ((result = ifs_ofs_efi_next(&v, &root, &dcur, &child, name)) == 0)
        ++entries;
    REQUIRE(result == -1);
    REQUIRE(entries == IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES);
    REQUIRE(read_calls <= 2U * (IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES + 1U));
    /* DOS\2 uses international Latin-1 case folding when hashing and
     * matching names; plain DOS\0 must not apply this transformation. */
    fixture();
    put(media[0], 0, 0x444f5302U);
    media[5][432] = 1U;
    media[5][433] = 0xe4U;
    checksum(media[5]);
    memset(media[4] + 6U * 4U, 0, 72U * 4U);
    {
        const uint8_t lower[] = { 0xe4U };
        const uint8_t upper[] = { 0xc4U };
        const uint32_t bucket = ifs_ofs_directory_hash(lower, 1U, 72U, 1);
        put(media[4], 6U + bucket, 5U);
        checksum(media[4]);
        REQUIRE(ifs_ofs_efi_mount(&v, COUNT, rd, NULL) == 0);
        REQUIRE((v.variant_flags & 0x02U) != 0U);
        root = ifs_ofs_efi_root(&v);
        REQUIRE(ifs_ofs_efi_lookup(&v, &root,
                                  (const uint8_t *)"boot", 4, &directory) == 0);
        REQUIRE(ifs_ofs_efi_lookup(&v, &directory, upper, 1, &file) == 0);
        REQUIRE(ifs_ofs_efi_read(&v, &file, 0, buf, 5, NULL) == 0);
        REQUIRE(memcmp(buf, "HELLO", 5) == 0);
    }
    /* DOS\4 directory-cache media still has the canonical directory hash
     * chain; the EFI reader must not write or use its optional cache blocks. */
    fixture();
    put(media[0], 0, 0x444f5304U);
    REQUIRE(ifs_ofs_efi_mount(&v, COUNT, rd, NULL) == 0);
    root = ifs_ofs_efi_root(&v);
    REQUIRE(ifs_ofs_efi_lookup(&v, &root,
                              (const uint8_t *)"BOOT", 4, &directory) == 0);
    REQUIRE(ifs_ofs_efi_lookup(&v, &directory,
                              (const uint8_t *)"kernel.efi", 10, &file) == 0);
    fixture();
    put(media[0], 0, 0x444f5301U); /* FFS must not bind as OFS. */
    REQUIRE(ifs_ofs_efi_mount(&v, COUNT, rd, NULL) == -1);
    put(media[0], 0, 0x444f5303U);
    REQUIRE(ifs_ofs_efi_mount(&v, COUNT, rd, NULL) == -1);
    put(media[0], 0, 0x444f5305U);
    REQUIRE(ifs_ofs_efi_mount(&v, COUNT, rd, NULL) == -1);
    /* The reader's address space is an unsigned 32-bit block count, not an
     * arbitrary INT32_MAX ceiling inherited from the Linux AFFS adapter. */
    {
        static const uint32_t sizes[] = { UINT32_C(0x80000100), UINT32_C(0x80000101), UINT32_MAX };
        size_t i;
        for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            IfsOfsEfiVolume big = {0};
            const uint32_t sectors = sizes[i];
            IfsOfsEfiEntry root_large, dir_large;
            REQUIRE(ifs_ofs_efi_mount(&big, sectors, rd_large,
                                      (void *)&sectors) == 0);
            REQUIRE(big.blocks == sectors);
            root_large = ifs_ofs_efi_root(&big);
            REQUIRE(ifs_ofs_efi_lookup(&big, &root_large,
                                      (const uint8_t *)"boot", 4U,
                                      &dir_large) == 0);
            REQUIRE(dir_large.block == LARGE_DIR);
        }
    }
    puts("OFS freestanding reader tests passed");
    return 0;
}
