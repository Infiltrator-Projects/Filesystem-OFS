/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent disposable in-memory OFS DOS\0 fixture. No physical disks. */
#include "ofs_module.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { BLOCKS = 1024, BLOCK = 512, ROOT = 512 };
static uint8_t disk[BLOCKS][BLOCK];

static void be32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}
static uint32_t load32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void put(uint8_t *b, uint32_t word, uint32_t x)
{
    be32(b + word * 4u, x);
}
static void checksum(uint8_t *b)
{
    uint32_t sum = 0;
    put(b, 5, 0);
    for (uint32_t i = 0; i < 128u; ++i)
        sum += load32(b + i * 4u);
    put(b, 5, 0u - sum);
}
static uint32_t hash(const char *name)
{
    uint32_t len = (uint32_t)strlen(name);
    uint32_t h = len;
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t c = (uint8_t)name[i];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        h = (h * 13u + c) & 0x7ffu;
    }
    return h % 72u;
}
static void name(uint8_t *b, const char *value)
{
    size_t length = strlen(value);
    b[432] = (uint8_t)length;
    memcpy(b + 433, value, length);
}
static int read_at(void *ctx, uint64_t offset, void *buffer, uint32_t len)
{
    (void)ctx;
    if (offset > sizeof disk || len > sizeof disk - offset)
        return -1;
    memcpy(buffer, (uint8_t *)disk + (size_t)offset, len);
    return 0;
}
static uint32_t writes, syncs;
static int write_at(void *ctx, uint64_t offset, const void *source, uint32_t len)
{
    (void)ctx;
    if (offset > sizeof disk || len > sizeof disk - offset)
        return -1;
    memcpy((uint8_t *)disk + (size_t)offset, source, len);
    ++writes;
    return 0;
}
static int sync_volume(void *ctx)
{
    (void)ctx;
    ++syncs;
    return 0;
}
static int reject_write(void *ctx, uint64_t offset, const void *source,
                        uint32_t len)
{
    (void)ctx; (void)offset; (void)source; (void)len;
    return -1;
}
static void fixture(void)
{
    uint8_t *root = disk[ROOT], *dir = disk[4],
            *file = disk[5], *data = disk[6];
    memset(disk, 0, sizeof disk);
    put(disk[0], 0, UINT32_C(0x444f5300));
    put(root, 0, 2);
    put(root, 3, 72);
    put(root, 127, 1);
    name(root, "OFSROOT");
    put(root, 6u + hash("boot"), 4);
    checksum(root);

    put(dir, 0, 2);
    put(dir, 1, 4);
    put(dir, 125, ROOT);
    put(dir, 127, 2);
    name(dir, "boot");
    put(dir, 6u + hash("kernel.efi"), 5);
    checksum(dir);

    put(file, 0, 2);
    put(file, 1, 5);
    put(file, 2, 1);
    put(file, 4, 6);
    put(file, 81, 5);
    put(file, 125, 4);
    put(file, 127, (uint32_t)-3);
    name(file, "kernel.efi");
    checksum(file);

    put(data, 0, 8);
    put(data, 1, 5);
    put(data, 2, 1);
    put(data, 3, 5);
    memcpy(data + 24, "HELLO", 5);
    checksum(data);
}
static int fail(const char *reason)
{
    fprintf(stderr, "ofs_dropin_module_test: %s\n", reason);
    return 1;
}
int main(void)
{
    const IfsOfsDropInModule *module = infiltrator_filesystem_module_v1();
    IfsOfsDropInIo io = {read_at, 0, sizeof disk};
    IfsOfsDropInVolume volume = {0};
    IfsOfsEfiEntry root, directory, file, listed;
    IfsOfsEfiDirCursor cursor = {0};
    IfsOfsEfiFileCursor read_cursor = {0};
    uint8_t filename[31] = {0}, data[8] = {0};

    if (!module || module->abi_version != 1 ||
        module->structure_bytes != sizeof *module ||
        strcmp(module->format_id, "ofs") != 0 ||
        (module->capabilities & (IFS_OFS_CAP_PROBE |
                                IFS_OFS_CAP_LOOKUP |
                                IFS_OFS_CAP_ENUMERATE |
                                IFS_OFS_CAP_FILE_READ |
                                IFS_OFS_CAP_DOS0)) == 0)
        return fail("missing OFS C ABI or read capabilities");
    fixture();
    if (module->mount(&volume, &io) != 0)
        return fail("failed to mount valid DOS0 fixture");
    root = module->root(&volume);
    if (root.block != ROOT ||
        module->lookup(&volume, &root, (const uint8_t *)"BOOT",
                       4, &directory) != 0)
        return fail("failed to locate directory by folded name");
    if (module->lookup(&volume, &directory,
                       (const uint8_t *)"kernel.efi",
                       10, &file) != 0 || file.size != 5)
        return fail("failed to find nested file");
    if (module->next(&volume, &directory, &cursor, &listed, filename) != 0 ||
        strcmp((const char *)filename, "kernel.efi") != 0 ||
        module->next(&volume, &directory, &cursor, &listed, filename) != 1)
        return fail("directory enumeration failed");
    if (module->read(&volume, &file, 0, data, 5, &read_cursor) != 0 ||
        memcmp(data, "HELLO", 5) != 0 ||
        module->read(&volume, &file, 2, data, 3, &read_cursor) != 0 ||
        memcmp(data, "LLO", 3) != 0)
        return fail("OFS file read incorrect");
    if (module->read(&volume, &file, 4, data, 2, 0) == 0)
        return fail("out of range read was accepted");


    {
        const IfsOfsDropInModuleV2 *writer = infiltrator_filesystem_module_v2();
        const uint8_t bytes[] = {'B', 'Y', 'E'};
        uint8_t original[512];
        if (!writer || writer->abi_version != 2U ||
            writer->structure_bytes != sizeof *writer ||
            strcmp(writer->format_id, "ofs") != 0 ||
            !(writer->capabilities & IFS_OFS_CAP_EXISTING_FILE_OVERWRITE))
            return fail("OFS V2 writer ABI missing");
        writes = syncs = 0U;
        if (writer->overwrite_existing(&volume, &file, 1U, bytes, 3U,
                                       write_at, sync_volume) != 0 ||
            writes != 1U || syncs != 1U)
            return fail("bounded OFS V2 writer failed");
        if (module->read(&volume, &file, 0, data, 5, 0) != 0 ||
            memcmp(data, "HBYEO", 5) != 0)
            return fail("OFS V2 writer did not persist validated data");
        memcpy(original, disk[6], sizeof original);
        if (writer->overwrite_existing(&volume, &file, 4U, bytes, 3U,
                                       write_at, sync_volume) == 0 ||
            writer->overwrite_existing(&volume, &directory, 0U, bytes, 1U,
                                       write_at, sync_volume) == 0 ||
            writer->overwrite_existing(&volume, &file, 0U, bytes, 1U,
                                       reject_write, sync_volume) == 0 ||
            writes != 1U ||
            memcmp(original, disk[6], sizeof original) != 0)
            return fail("OFS V2 violated bounds or modified rejected writes");
        disk[6][25] ^= 1U;
        if (writer->overwrite_existing(&volume, &file, 0U, bytes, 1U,
                                       write_at, sync_volume) == 0 ||
            writes != 1U)
            return fail("OFS V2 accepted corrupt data chain");
        fixture();
        put(disk[0], 0, UINT32_C(0x444f5301)); /* Hot-swapped FFS after mount. */
        if (writer->overwrite_existing(&volume, &file, 0U, bytes, 1U,
                                       write_at, sync_volume) == 0 ||
            writes != 1U)
            return fail("OFS V2 wrote stale/foreign-format mounted media");
        fixture();
        if (module->mount(&volume, &io) != 0 ||
            module->lookup(&volume, &(IfsOfsEfiEntry){ROOT, 0, IFS_OFS_EFI_DIRECTORY},
                           (const uint8_t *)"boot", 4, &directory) != 0 ||
            module->lookup(&volume, &directory,
                           (const uint8_t *)"kernel.efi", 10, &file) != 0)
            return fail("OFS V2 test fixture reset failed");
    }

    /* A real DOS0 data chain spans 488-byte payload blocks. Verify a
     * cross-boundary overwrite, independent readback, and whole-span
     * preflight on a deliberately corrupted trailing block. */
    {
        const IfsOfsDropInModuleV2 *writer = infiltrator_filesystem_module_v2();
        const uint8_t payload[] = {'X','Y','Z','W'};
        uint8_t out[sizeof payload] = {0};
        uint32_t count_before;
        fixture();
        put(disk[5], 2, 2);
        put(disk[5], 81, 600);
        checksum(disk[5]);
        put(disk[6], 3, 488);
        put(disk[6], 4, 7);
        memset(disk[6]+24, 'A', 488);
        checksum(disk[6]);
        put(disk[7], 0, 8);
        put(disk[7], 1, 5);
        put(disk[7], 2, 2);
        put(disk[7], 3, 112);
        memset(disk[7]+24, 'B', 112);
        checksum(disk[7]);
        if (module->mount(&volume, &io) != 0)
            return fail("cross-block OFS fixture mount");
        root = module->root(&volume);
        if (module->lookup(&volume, &root, (const uint8_t *)"boot", 4,
                           &directory) != 0 ||
            module->lookup(&volume, &directory,
                           (const uint8_t *)"kernel.efi", 10, &file) != 0 ||
            file.size != 600)
            return fail("cross-block fixture lookup");
        writes = syncs = 0;
        if (writer->overwrite_existing(&volume, &file, 487, payload, 4,
                                       write_at, sync_volume) != 0 ||
            writes != 2 || syncs != 2 ||
            module->read(&volume, &file, 487, out, 4, 0) != 0 ||
            memcmp(out, payload, 4) != 0)
            return fail("OFS cross-data-block mutation and persistence");
        count_before = writes;
        disk[7][30] ^= 1u;
        if (writer->overwrite_existing(&volume, &file, 487, payload, 4,
                                       write_at, sync_volume) == 0 ||
            writes != count_before)
            return fail("OFS writer partially modified corrupted tail");
        fixture();
        if (module->mount(&volume, &io) != 0)
            return fail("OFS cross-block fixture reset");
        root = module->root(&volume);
        if (module->lookup(&volume, &root, (const uint8_t *)"boot", 4,
                           &directory) != 0 ||
            module->lookup(&volume, &directory,
                           (const uint8_t *)"kernel.efi", 10, &file) != 0)
            return fail("OFS cross-block reset lookup");
    }
    disk[6][25] ^= 1;
    if (module->read(&volume, &file, 0, data, 5, 0) == 0)
        return fail("corrupt data checksum was accepted");
    fixture();
    put(disk[0], 0, UINT32_C(0x444f5301));
    if (module->mount(&volume, &io) == 0)
        return fail("FFS DOS1 mounted as OFS");
    fixture();
    put(disk[0], 0, UINT32_C(0x444f5302));
    if (module->mount(&volume, &io) != 0)
        return fail("international OFS DOS2 rejected");
    fixture();
    put(disk[0], 0, UINT32_C(0x444f5304));
    if (module->mount(&volume, &io) != 0)
        return fail("cache OFS DOS4 rejected");
    fixture();
    io.volume_bytes = 512;
    if (module->mount(&volume, &io) == 0)
        return fail("truncated volume accepted");
    puts("OFS-DROPIN-READ-AND-IDENTITY-PASS");
    return 0;
}
