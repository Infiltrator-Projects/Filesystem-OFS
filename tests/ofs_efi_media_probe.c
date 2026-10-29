/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Read-only physical-media reproduction of the firmware OFS directory reader.
 * This is a diagnostic, not an independent filesystem implementation. All
 * format interpretation remains in the canonical Filesystems EFI reader.
 *
 * Open the partition or image with O_RDONLY; every callback is pread(2).
 * No mounts, ioctl mutations, filesystem repairs or writes are performed.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "../native/filesystems/ofs/efi/reader.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { SECTOR_BYTES = 512, MAX_ITEMS = IFS_OFS_EFI_MAX_DIRECTORY_ENTRIES };

static int media_fd = -1;
static uint64_t reads;

static int read_sector(void *unused, uint32_t block, uint8_t data[512])
{
    ssize_t count;
    (void)unused;
    ++reads;
    count = pread(media_fd, data, SECTOR_BYTES, (off_t)block * SECTOR_BYTES);
    if (count != SECTOR_BYTES) {
        fprintf(stderr, "Physical read failed: block %" PRIu32
                ", result %zd, errno %d\n", block, count,
                count < 0 ? errno : 0);
        return -1;
    }
    return 0;
}

static void print_name(const uint8_t name[31])
{
    size_t i;
    for (i = 0; i < 30U && name[i] != 0U; ++i) {
        const uint8_t c = name[i];
        if (c >= 0x20U && c <= 0x7eU)
            putchar((int)c);
        else
            printf("\\x%02x", c);
    }
}

static int scan_directory(const IfsOfsEfiVolume *volume,
                          const IfsOfsEfiEntry *directory, const char *name)
{
    IfsOfsEfiDirCursor cursor = {0};
    IfsOfsEfiEntry entry;
    uint8_t child_name[31];
    unsigned count;
    int result;
    uint64_t before = reads;

    printf("Directory: %s (metadata block %" PRIu32 ")\n",
           name, directory->block);
    for (count = 0; count <= MAX_ITEMS; ++count) {
        result = ifs_ofs_efi_next(volume, directory, &cursor, &entry,
                                  child_name);
        if (result == 1) {
            printf("PASS: %s enumerated %u entries (%" PRIu64
                   " underlying sector reads)\n", name, count, reads - before);
            return 0;
        }
        if (result != 0) {
            fprintf(stderr, "FAIL: canonical OFS reader rejected directory "
                    "%s at entry %u (hash bucket %" PRIu32
                    ", chain block %" PRIu32 ", traversed %" PRIu32 ")\n",
                    name, count, cursor.bucket, cursor.next_block,
                    cursor.visited);
            return 1;
        }
        if (count == MAX_ITEMS) {
            fprintf(stderr, "FAIL: reader returned more than %u entries\\n", MAX_ITEMS);
            return 1;
        }
        printf("  [%u] ", count + 1U);
        print_name(child_name);
        printf(" (block %" PRIu32 ", size %" PRIu32
               ", %s)\n", entry.block, entry.size,
               entry.type == IFS_OFS_EFI_DIRECTORY ? "directory" : "file");
    }
    fprintf(stderr, "FAIL: stopped at %u entries; reader's bounded "
            "enumeration limit was reached\n", MAX_ITEMS);
    return 1;
}

int main(int argc, char **argv)
{
    off_t end;
    uint64_t blocks;
    IfsOfsEfiVolume volume = {0};
    IfsOfsEfiEntry root, boot;
    const char *source;
    int found;
    int failed = 0;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s /dev/partition-or-image\n", argv[0]);
        return 2;
    }
    source = argv[1];
    media_fd = open(source, O_RDONLY | O_CLOEXEC);
    if (media_fd < 0) {
        perror(source);
        return 2;
    }
    end = lseek(media_fd, 0, SEEK_END);
    if (end < 0 || (uint64_t)end % SECTOR_BYTES != 0U ||
        (uint64_t)end / SECTOR_BYTES > UINT32_MAX) {
        fprintf(stderr, "Unsupported or unknown partition size\n");
        close(media_fd);
        return 2;
    }
    blocks = (uint64_t)end / SECTOR_BYTES;
    printf("Read-only OFS inspection: %s (%" PRIu64 " blocks)\n",
           source, blocks);
    if (ifs_ofs_efi_mount(&volume, (uint32_t)blocks,
                          read_sector, NULL) != 0) {
        fprintf(stderr, "FAIL: canonical OFS EFI reader rejected "
                "partition boot/root metadata\n");
        close(media_fd);
        return 1;
    }
    root = ifs_ofs_efi_root(&volume);
    failed |= scan_directory(&volume, &root, "/");
    found = ifs_ofs_efi_lookup(&volume, &root,
                               (const uint8_t *)"boot", 4U, &boot);
    if (found == 0 && boot.type == IFS_OFS_EFI_DIRECTORY)
        failed |= scan_directory(&volume, &boot, "/boot");
    else {
        fprintf(stderr, "FAIL: unable to resolve /boot through canonical "
                "EFI reader (result %d, type %d)\n",
                found, found == 0 ? boot.type : -1);
        failed = 1;
    }
    close(media_fd);
    printf("Read calls: %" PRIu64 "; media left untouched\n", reads);
    return failed ? 1 : 0;
}
