/* SPDX-License-Identifier: GPL-3.0-or-later
 * Compare an externally formatted OFS disk against independent source files.
 * Only the Common V2 entry point is visible to this test host.
 */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct {
    int fd;
    uint32_t sector;
} Disk;

static const IfsFsCoreOpsV2 *unmount_during_read;
static void *active_volume;
static int observed_unmount;
static int read_disk(void *context, uint64_t offset, void *out, uint32_t count)
{
    if (unmount_during_read) {
        const IfsFsCoreOpsV2 *ops=unmount_during_read;
        unmount_during_read=NULL;
        observed_unmount=ops->unmount(active_volume);
        if (observed_unmount!=IFS_FS_BUSY) return -1;
    }
    Disk *disk = context;
    if (!count || count > 16384 || offset % disk->sector || count % disk->sector)
        return -1;
    return pread(disk->fd, out, count, (off_t)offset) == (ssize_t)count ? 0 : -1;
}

static void *allocate(void *context, size_t bytes) { (void)context; return malloc(bytes); }
static void release(void *context, void *p) { (void)context; free(p); }

static void compare_file(const IfsFsCoreOpsV2 *core, void *volume, const char *path,
                         const char *source)
{
    int fd = open(source, O_RDONLY);
    struct stat ref;
    char name[128];
    IfsFsEntryV2 entry = {.structure_bytes = sizeof(entry), .name = name, .name_capacity = sizeof(name)};
    REQUIRE(fd >= 0 && fstat(fd, &ref) == 0 && ref.st_size > 0);
    REQUIRE(core->stat(volume, path, &entry) == IFS_FS_OK);
    REQUIRE(entry.type == IFS_FS_ENTRY_FILE && entry.size_bytes == (uint64_t)ref.st_size);
    uint8_t expected[32767], actual[sizeof(expected)];
    for (uint64_t offset = 0; offset < entry.size_bytes;) {
        uint64_t left = entry.size_bytes - offset;
        uint32_t count = left < sizeof(expected) ? (uint32_t)left : sizeof(expected);
        REQUIRE(pread(fd, expected, count, (off_t)offset) == (ssize_t)count);
        REQUIRE(core->read(volume, path, offset, actual, count) == IFS_FS_OK);
        REQUIRE(memcmp(expected, actual, count) == 0);
        offset += count;
    }
    REQUIRE(close(fd) == 0);
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 7);
    void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    REQUIRE(image);
    void *symbol = dlsym(image, IFS_FS_COMMON_EXPORT_V2);
    IfsFsModuleApiEntryV2 entry;
    REQUIRE(symbol && sizeof(symbol) == sizeof(entry));
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsFsModuleApiV2 *api = entry();
    REQUIRE(ifs_fs_api_v2_valid(api, "ofs") && (api->runtime_profiles & IFS_FS_PROFILE_FIRMWARE));
    const IfsFsCoreOpsV2 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    Disk disk = {open(argv[2], O_RDONLY), (uint32_t)strtoul(argv[3], NULL, 10)};
    struct stat media;
    REQUIRE(disk.fd >= 0 && fstat(disk.fd, &media) == 0);
    IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2, .structure_bytes = IFS_FS_HOST_V2_BASE_BYTES,
        .capabilities = IFS_FS_HOST_READ_ONLY, .volume_bytes = (uint64_t)media.st_size,
        .logical_sector_bytes = disk.sector, .physical_sector_bytes = disk.sector,
        .maximum_transfer_bytes = 16384, .context = &disk,
        .read_at = read_disk, .allocate = allocate, .release = release};
    uint32_t variant = UINT32_MAX;
    IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
    void *volume = NULL;
    REQUIRE(core->probe(&host, &variant) == IFS_FS_OK && variant == 0);
    REQUIRE(core->mount(&host, &volume, &info) == IFS_FS_OK && volume);
    REQUIRE(info.max_name_bytes == 60u);
    const IfsFsVolumeOpsV2 *ops = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    REQUIRE(ops && ops->query && ops->query(volume,&info)==IFS_FS_OK);
    REQUIRE(info.max_name_bytes == 60u);
    if (!strcmp(argv[4], "bad-data")) {
        uint8_t byte = 0;
        REQUIRE(core->read(volume, "/boot/vmlinuz", 0, &byte, 1) == IFS_FS_CORRUPT);
    } else {
        REQUIRE(!strcmp(argv[4], "clean"));
        char name[128];
        IfsFsEntryV2 file = {.structure_bytes = sizeof(file), .name = name, .name_capacity = sizeof(name)};
        unsigned seen = 0;
        REQUIRE(core->stat(volume, "/boot/missing", &file) == IFS_FS_NOT_FOUND);
        for (uint32_t i = 0; ; ++i) {
            int rc = core->directory_entry(volume, "/boot", i, &file);
            if (rc == IFS_FS_END) break;
            REQUIRE(rc == IFS_FS_OK && i < 2 && file.type == IFS_FS_ENTRY_FILE);
            if (!strcmp(name, "vmlinuz")) seen |= 1;
            else if (!strcmp(name, "initramfs")) seen |= 2;
            else REQUIRE(0);
        }
        REQUIRE(seen == 3);
        /* Some chunks can be served entirely from the native cache.
         * Arm once across the full independent payload comparison, so
         * only an actual host read must observe the reentrant BUSY. */
        active_volume=volume;
        unmount_during_read=core;
        observed_unmount=IFS_FS_INVALID;
        compare_file(core, volume, "/boot/vmlinuz", argv[5]);
        compare_file(core, volume, "/boot/initramfs", argv[6]);
        REQUIRE(!unmount_during_read && observed_unmount==IFS_FS_BUSY);
    }
    REQUIRE(core->unmount(volume) == IFS_FS_OK);
    REQUIRE(close(disk.fd) == 0 && dlclose(image) == 0);
    return 0;
}
