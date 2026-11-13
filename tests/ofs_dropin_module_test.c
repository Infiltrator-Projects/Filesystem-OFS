/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent disposable in-memory OFS DOS\0 fixture. No physical disks. */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);

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
static void *allocate(void *ctx, size_t bytes)
{
    (void)ctx;
    return malloc(bytes);
}
static void release(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
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
    put(root, 78, UINT32_MAX);
    put(root, 79, 2);
    name(root, "OFSROOT");
    put(root, 6u + hash("boot"), 4);
    checksum(root);

    /* Valid fixtures include the allocation bitmap and reversed data table. */
    for (uint32_t key = 2; key < BLOCKS; ++key)
        if (key != 2 && key != ROOT && (key < 4 || key > 7))
            put(disk[2], 1 + (key-2)/32,
                load32(disk[2] + (1 + (key-2)/32)*4) | (1u << ((key-2)%32)));
    {
        uint32_t sum=0;
        for (uint32_t i=1;i<128;i++) sum+=load32(disk[2]+i*4);
        put(disk[2],0,0u-sum);
    }

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
    put(file, 77, 6);
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
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core =
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume_ops =
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    if (!ifs_fs_api_v2_valid(api, "ofs") || !core || !volume_ops ||
        !core->probe || !core->mount || !core->unmount || !core->stat ||
        !core->directory_entry || !core->read || !core->write || !volume_ops->sync)
        return fail("Common V2 capabilities missing");

    const IfsFsHostRuntimeV2 read_only = {
        .abi_version = IFS_FS_HOST_ABI_V2,
        .structure_bytes = sizeof(IfsFsHostRuntimeV2),
        .capabilities = IFS_FS_HOST_READ_ONLY,
        .volume_bytes = sizeof(disk),
        .logical_sector_bytes = BLOCK,
        .physical_sector_bytes = BLOCK,
        .maximum_transfer_bytes = 65536u,
        .read_at = read_at,
        .allocate = allocate,
        .release = release
    };
    IfsFsHostRuntimeV2 writable = read_only;
    writable.capabilities = IFS_FS_HOST_EXCLUSIVE | IFS_FS_HOST_DURABLE;
    writable.write_at = write_at;
    writable.flush = sync_volume;
    IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
    void *volume = NULL;
    char entry_name[64];
    IfsFsEntryV2 entry = {
        .structure_bytes = sizeof(entry), .name = entry_name,
        .name_capacity = sizeof(entry_name)
    };
    const char *path = "/BOOT/KERNEL.EFI";
    char data[8];
    uint32_t variant = UINT32_MAX;

    fixture();
    if (core->probe(&read_only, &variant) != IFS_FS_OK || variant != 0 ||
        core->mount(&read_only, &volume, &info) != IFS_FS_OK ||
        core->stat(volume, path, &entry) != IFS_FS_OK ||
        entry.type != IFS_FS_ENTRY_FILE || entry.size_bytes != 5 ||
        core->directory_entry(volume, "/BOOT", 0, &entry) != IFS_FS_OK ||
        strcmp(entry.name, "kernel.efi") != 0 ||
        core->directory_entry(volume, "/BOOT", 1, &entry) != IFS_FS_END ||
        core->read(volume, path, 0, data, 5) != IFS_FS_OK ||
        memcmp(data, "HELLO", 5) != 0 ||
        core->read(volume, path, 2, data, 3) != IFS_FS_OK ||
        memcmp(data, "LLO", 3) != 0 ||
        core->read(volume, path, 4, data, 2) == IFS_FS_OK ||
        core->write(volume, path, 0, "X", 1) != IFS_FS_ACCESS ||
        core->unmount(volume) != IFS_FS_OK || writes != 0)
        return fail("read-only nested file, enumeration or bounds");

    /* Extend the independent fixture to two OFS data blocks. The write
     * crosses their payload boundary and must survive an unmount. */
    put(disk[5], 2, 2);
    put(disk[5], 76, 7);
    put(disk[5], 81, 600);
    checksum(disk[5]);
    put(disk[6], 3, 488);
    put(disk[6], 4, 7);
    memset(disk[6] + 24, 'A', 488);
    checksum(disk[6]);
    put(disk[7], 0, 8);
    put(disk[7], 1, 5);
    put(disk[7], 2, 2);
    put(disk[7], 3, 112);
    memset(disk[7] + 24, 'B', 112);
    checksum(disk[7]);

    if (core->mount(&writable, &volume, &info) != IFS_FS_OK ||
        core->write(volume, path, 487, "XYZW", 4) != IFS_FS_OK ||
        volume_ops->sync(volume) != IFS_FS_OK ||
        core->unmount(volume) != IFS_FS_OK || writes == 0 || syncs == 0 ||
        core->mount(&read_only, &volume, &info) != IFS_FS_OK ||
        core->read(volume, path, 487, data, 4) != IFS_FS_OK ||
        memcmp(data, "XYZW", 4) != 0 || core->unmount(volume) != IFS_FS_OK ||
        disk[6][511] != 'X' || memcmp(disk[7] + 24, "YZW", 3) != 0)
        return fail("cross-block overwrite or persistence");

    /* Writable Common mounts validate ownership and data before opening
     * the volume to updates. The backing image is exclusive while mounted. */
    const uint32_t prior_writes = writes;
    disk[7][30] ^= 1u;
    if (core->mount(&writable, &volume, &info) == IFS_FS_OK ||
        writes != prior_writes || disk[6][511] != 'X')
        return fail("corrupt tail accepted for writes");

    fixture();
    put(disk[0], 0, UINT32_C(0x444f5301));
    if (core->probe(&read_only, &variant) == IFS_FS_OK)
        return fail("foreign FFS detected as OFS");
    IfsFsHostRuntimeV2 truncated = read_only;
    truncated.volume_bytes = BLOCK;
    if (core->mount(&truncated, &volume, &info) == IFS_FS_OK)
        return fail("truncated volume accepted");
    puts("OFS-COMMON-V2-FIXTURE-READ-WRITE-PASS");
    return 0;
}
