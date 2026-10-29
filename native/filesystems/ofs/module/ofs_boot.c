/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only Boot V1 view of the same OFS engine and module image as V3.
 * The host supplies partition-relative bytes, not an OFS implementation.
 */
#include "../core/ofs_engine.h"
#include "infiltratr/fs/boot_module.h"
extern OfsFs ifs_ofs_active;
typedef struct {
    OfsNode node;
    OfsDirCursor directory;
    OfsFileCursor file;
    uint32_t index;
    int valid;
} BootFile;
static IfsBootIo device;
static int mounted;
static uint32_t open_files;
__attribute__((used, section(".ifsboot"))) static const IfsBootManifest manifest = {
    {'I', 'F', 'S', 'B', 'O', 'O', 'T', '1'},
    sizeof(IfsBootManifest),
    IFS_BOOT_ABI,
    62,
    IFS_BOOT_PROFILE_X64,
    "ofs",
    "Amiga Original File System"};
static int read_at(void *unused, uint64_t at, void *out, uint32_t n) {
    (void)unused;
    if (at > device.media_bytes || n > device.media_bytes - at)
        return -1;
    return device.read_at(device.context, at, out, n);
}
static int status(int r) {
    if (r == 0)
        return IFS_BOOT_OK;
    if (r == IFS_OFS_END)
        return IFS_BOOT_END;
    if (r == IFS_OFS_NOT_FOUND)
        return IFS_BOOT_NOT_FOUND;
    return r == IFS_OFS_IO_ERROR ? IFS_BOOT_IO_ERROR : IFS_BOOT_INVALID;
}
static int directory(const OfsNode *n) {
    return n->object_type == IFS_OFS_ROOT || n->object_type == IFS_OFS_DIRECTORY;
}
static void info_copy(const OfsNode *n, IfsBootInfo *out) {
    ofs_zero(out, sizeof(*out));
    out->bytes = n->bytes;
    out->attributes = directory(n) ? IFS_BOOT_DIRECTORY : 0;
    if (n->protection & 4u)
        out->attributes |= 1u;
    if (n->type == IFS_OFS_ROOT) {
        out->name[0] = '/';
        out->name[1] = 0;
    } else
        (void)ofs_utf8(n->name, n->name_bytes, out->name, sizeof(out->name));
}
static int IFS_BOOT_CALL mount_volume(const IfsBootIo *io) {
    if (!io || io->structure_bytes != sizeof(*io) || !io->read_at || mounted)
        return IFS_BOOT_INVALID;
    device = *io;
    IfsOfsIoV3 input = {
        sizeof(input), IFS_OFS_READ_ONLY, 0, io->media_bytes, read_at, 0, 0, 0, 0, 0};
    int r = ofs_probe(&ifs_ofs_active, &input, 0);
    if (r)
        return status(r);
    mounted = 1;
    return 0;
}
static int IFS_BOOT_CALL unmount_volume(void) {
    if (!mounted || open_files)
        return IFS_BOOT_INVALID;
    ofs_release(&ifs_ofs_active);
    mounted = 0;
    return 0;
}
static int IFS_BOOT_CALL open_file(const char *path, void *context, IfsBootInfo *out) {
    BootFile *file = context;
    if (!mounted || !path || !file || !out || file->valid || open_files >= 256u)
        return IFS_BOOT_INVALID;
    OfsNode node;
    int r = ofs_path(&ifs_ofs_active, path, 1, &node);
    if (r)
        return status(r);
    if (!directory(&node) && node.object_type != IFS_OFS_FILE)
        return IFS_BOOT_INVALID;
    ofs_zero(file, sizeof(*file));
    file->node = node;
    file->valid = 1;
    info_copy(&node, out);
    ++open_files;
    return 0;
}
static int IFS_BOOT_CALL close_file(void *context) {
    BootFile *file = context;
    if (!mounted || !file || !file->valid)
        return IFS_BOOT_INVALID;
    file->valid = 0;
    --open_files;
    return 0;
}
static int IFS_BOOT_CALL read_file(void *context, uint64_t at, void *out, uint32_t n,
                                   uint32_t *got) {
    BootFile *file = context;
    if (got)
        *got = 0;
    if (!mounted || !file || !file->valid || directory(&file->node) || !got || (!out && n) ||
        at > file->node.bytes)
        return IFS_BOOT_INVALID;
    uint32_t count = n;
    if (count > file->node.bytes - (uint32_t)at)
        count = file->node.bytes - (uint32_t)at;
    int r = ofs_read_file(&ifs_ofs_active, &file->node, (uint32_t)at, out, count, &file->file);
    if (!r)
        *got = count;
    return status(r);
}
static int IFS_BOOT_CALL next_entry(void *context, uint32_t index, IfsBootInfo *out) {
    BootFile *file = context;
    if (!mounted || !file || !file->valid || !directory(&file->node) || !out)
        return IFS_BOOT_INVALID;
    if (index < file->index) {
        ofs_zero(&file->directory, sizeof(file->directory));
        file->index = 0;
    }
    OfsNode node;
    while (file->index <= index) {
        int r = ofs_next(&ifs_ofs_active, file->node.object, &file->directory, &node);
        if (r)
            return status(r);
        if (file->index == UINT32_MAX)
            return IFS_BOOT_INVALID;
        ++file->index;
    }
    info_copy(&node, out);
    return 0;
}
static int IFS_BOOT_CALL volume_label(char *out, uint32_t bytes) {
    if (!mounted || !out || !bytes)
        return IFS_BOOT_INVALID;
    OfsNode root;
    int r = ofs_node(&ifs_ofs_active, ifs_ofs_active.root, &root, ifs_ofs_active.scratch[1]);
    return r ? status(r) : status(ofs_utf8(root.name, root.name_bytes, out, bytes));
}
static const IfsBootModuleV1 module = {
    IFS_BOOT_ABI,   sizeof(module), "ofs",      sizeof(BootFile), 0,          mount_volume,
    unmount_volume, open_file,      close_file, read_file,        next_entry, volume_label};
__attribute__((visibility("default"))) const IfsBootModuleV1 *IFS_BOOT_CALL
infiltrator_filesystem_boot_v1(void) {
    return &module;
}
