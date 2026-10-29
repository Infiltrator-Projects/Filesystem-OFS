/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ofs_v3.h"
#include "../core/ofs_engine.h"

/* A loaded module image owns one serialized mount; the Boot host clones the
 * same ELF image for another volume. No process-global or firmware imports. */
OfsFs ifs_ofs_active;
static int mount_volume(const IfsOfsIoV3 *io, const IfsOfsMountV3 *options) {
    return ofs_probe(&ifs_ofs_active, io, options);
}
static int unmount_volume(void) {
    OfsFs *f = &ifs_ofs_active;
    if (!f->mounted)
        return IFS_OFS_INVALID;
    int r = 0;
    if (!(f->io.flags & IFS_OFS_READ_ONLY) && f->io.flush(f->io.context))
        r = IFS_OFS_IO_ERROR;
    ofs_release(f);
    return r;
}
static int format_volume(const IfsOfsIoV3 *io, const IfsOfsFormatV3 *options) {
    if (ifs_ofs_active.mounted)
        return IFS_OFS_INVALID;
    return ofs_format(&ifs_ofs_active, io, options);
}
static int stat_path(const char *path, IfsOfsEntryV3 *out) {
    OfsNode n;
    int r = ofs_path(&ifs_ofs_active, path, 1, &n);
    return r ? r : ofs_entry(&ifs_ofs_active, &n, out);
}
static int lstat_path(const char *path, IfsOfsEntryV3 *out) {
    OfsNode n;
    int r = ofs_path(&ifs_ofs_active, path, 0, &n);
    return r ? r : ofs_entry(&ifs_ofs_active, &n, out);
}
static int nth(const char *path, uint32_t index, IfsOfsEntryV3 *out) {
    OfsFs *f = &ifs_ofs_active;
    OfsNode dir, n;
    int r = ofs_path(f, path, 1, &dir);
    if (r)
        return r;
    if (dir.object_type != IFS_OFS_ROOT && dir.object_type != IFS_OFS_DIRECTORY)
        return IFS_OFS_INVALID;
    OfsDirCursor c = {0};
    for (uint64_t i = 0; i <= (uint64_t)index; ++i) {
        r = ofs_next(f, dir.object, &c, &n);
        if (r)
            return r;
    }
    return ofs_entry(f, &n, out);
}
static int create_file(const char *p) { return ofs_create(&ifs_ofs_active, p, IFS_OFS_FILE, 0, 0); }
static int mkdir_path(const char *p) {
    return ofs_create(&ifs_ofs_active, p, IFS_OFS_DIRECTORY, 0, 0);
}
static int read_file(const char *p, uint32_t at, void *b, uint32_t n) {
    OfsNode file;
    int r = ofs_path(&ifs_ofs_active, p, 1, &file);
    return r ? r : ofs_read_file(&ifs_ofs_active, &file, at, b, n, 0);
}
static int grow_check(OfsFs *f, uint32_t old, uint32_t bytes) {
    if (bytes <= old)
        return 0;
    uint32_t payload = f->block_bytes - 24u;
    uint64_t a = old ? (old - 1u) / payload + 1u : 0, b = (bytes - 1u) / payload + 1u;
    uint64_t ae = a ? (a - 1u) / f->hash_size : 0, be = (b - 1u) / f->hash_size;
    IfsOfsSpaceV3 space;
    int r = ofs_space(f, &space);
    if (r)
        return r;
    return b - a + be - ae <= space.free_bytes / f->block_bytes ? 0 : IFS_OFS_NO_SPACE;
}
static int write_file(const char *p, uint32_t at, const void *b, uint32_t n) {
    OfsFs *f = &ifs_ofs_active;
    OfsNode file;
    if ((!b && n) || n > UINT32_MAX - at)
        return IFS_OFS_INVALID;
    int r = ofs_path(f, p, 1, &file);
    if (r)
        return r;
    if (file.object_type != IFS_OFS_FILE)
        return IFS_OFS_INVALID;
    if (file.protection & 4u)
        return IFS_OFS_ACCESS;
    if (!n)
        return 0;
    r = ofs_check_file(f, &file);
    if (r)
        return r;
    r = grow_check(f, file.bytes, at + n);
    if (r)
        return r;
    r = ofs_begin(f);
    if (r)
        return r;
    return ofs_end(f, ofs_write_file(f, &file, at, b, n));
}
static int truncate_file(const char *p, uint32_t bytes) {
    OfsFs *f = &ifs_ofs_active;
    OfsNode file;
    int r = ofs_path(f, p, 1, &file);
    if (r)
        return r;
    if (file.object_type != IFS_OFS_FILE)
        return IFS_OFS_INVALID;
    if (file.protection & 4u)
        return IFS_OFS_ACCESS;
    r = ofs_check_file(f, &file);
    if (r)
        return r;
    r = grow_check(f, file.bytes, bytes);
    if (r)
        return r;
    r = ofs_begin(f);
    if (r)
        return r;
    return ofs_end(f, ofs_resize(f, &file, bytes));
}
static int rename_path(const char *a, const char *b) { return ofs_rename(&ifs_ofs_active, a, b); }
static int unlink_path(const char *p) { return ofs_unlink(&ifs_ofs_active, p); }
static int protect(const char *p, uint32_t flags) {
    return ofs_set_metadata(&ifs_ofs_active, p, 1, &flags);
}
static int owner(const char *p, uint16_t uid, uint16_t gid) {
    uint16_t ids[2] = {uid, gid};
    return ofs_set_metadata(&ifs_ofs_active, p, 2, ids);
}
static int comment(const char *p, const char *text) {
    return ofs_set_metadata(&ifs_ofs_active, p, 3, text);
}
static int dates(const char *p, const IfsOfsDate *date, uint32_t mask) {
    if (!mask || (mask & ~7u))
        return IFS_OFS_INVALID;
    return ofs_set_metadata(&ifs_ofs_active, p, (mask << 8) | 4u, date);
}
static int label(const char *p) { return ofs_set_label(&ifs_ofs_active, p); }
static int space(IfsOfsSpaceV3 *out) { return ofs_space(&ifs_ofs_active, out); }
static int hardlink(const char *old, const char *path) {
    OfsNode n;
    int r = ofs_path(&ifs_ofs_active, old, 1, &n);
    return r ? r : ofs_create(&ifs_ofs_active, path, IFS_OFS_LINKFILE, 0, n.object);
}
static int symlink(const char *target, const char *path) {
    return ofs_create(&ifs_ofs_active, path, IFS_OFS_SOFTLINK, target, 0);
}
static int readlink(const char *path, char *out, uint32_t n) {
    return ofs_readlink(&ifs_ofs_active, path, out, n);
}
static int verify(IfsOfsCheckV3 *out) { return ofs_verify(&ifs_ofs_active, out, 0); }
static int recover(IfsOfsCheckV3 *out) { return ofs_verify(&ifs_ofs_active, out, 1); }
static int flush(void) {
    OfsFs *f = &ifs_ofs_active;
    if (!f->mounted)
        return IFS_OFS_INVALID;
    return f->io.flags & IFS_OFS_READ_ONLY   ? 0
           : f->io.flush(f->io.context) == 0 ? 0
                                             : IFS_OFS_IO_ERROR;
}
__attribute__((used, section(".ifsmeta"))) static const char identity[] = "IFS-MODULE:ofs:3";
static const IfsOfsModuleV3 module = {IFS_OFS_ABI_V3,
                                      sizeof(IfsOfsModuleV3),
                                      "ofs",
                                      IFS_OFS_CAP_FULL_RW | IFS_OFS_CAP_FORMAT |
                                          IFS_OFS_CAP_METADATA | IFS_OFS_CAP_LINKS |
                                          IFS_OFS_CAP_VERIFY | IFS_OFS_CAP_RECOVER_BITMAP |
                                          IFS_OFS_CAP_ALL_BLOCK_SIZES | IFS_OFS_CAP_UNSIGNED_KEYS,
                                      mount_volume,
                                      unmount_volume,
                                      format_volume,
                                      stat_path,
                                      nth,
                                      create_file,
                                      mkdir_path,
                                      read_file,
                                      write_file,
                                      truncate_file,
                                      rename_path,
                                      unlink_path,
                                      protect,
                                      owner,
                                      comment,
                                      dates,
                                      label,
                                      space,
                                      hardlink,
                                      symlink,
                                      readlink,
                                      verify,
                                      recover,
                                      lstat_path,
                                      flush};
__attribute__((visibility("default"))) const IfsOfsModuleV3 *
infiltrator_filesystem_module_v3(void) {
    return &module;
}
