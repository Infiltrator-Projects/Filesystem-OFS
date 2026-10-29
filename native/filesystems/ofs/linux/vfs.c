// SPDX-License-Identifier: GPL-3.0-or-later
/* Linux VFS binding for the canonical OFS engine.
 *
 * s_blocksize is a device-buffer unit, never the filesystem's B. The engine
 * receives bounded byte I/O and owns all DOS0/2/4/6 interpretation and mutation,
 * including blocks larger than PAGE_SIZE. One mutex serializes each mounted
 * engine; separate superblocks have independent engine state and caches.
 *
 * Ordinary writes complete through the engine before write_end returns. Shared
 * mmap dirties folios normally and writepages feeds them through the same byte
 * writer. No on-disk OFS block is mapped directly as a Linux page. Open unlinked
 * files use the engine's deferred deletion and keep the volume dirty until the
 * last inode reference is evicted; they never create hidden on-disk filenames.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/magic.h>
#include <linux/xattr.h>
#include <linux/uaccess.h>
#include <linux/timekeeping.h>
#include <linux/parser.h>
#include <linux/namei.h>
#include <linux/iversion.h>
#include "../core/ofs_engine.h"
#include "../../../platform/linux_kernel/vfs_compat.h"

#define OFS_EPOCH 252460800LL
struct ofs_mount {
    struct super_block *sb;
    struct mutex lock;
    OfsFs *fs;
    int error;
    u32 uid, gid;
    umode_t mode;
    bool force_uid, force_gid, force_mode, protect, mufs, verbose;
    char *prefix;
    char volume[32];
};
struct ofs_inode {
    u32 object;
    u32 protection;
    struct inode inode;
};
static const struct inode_operations ofs_file_iops, ofs_dir_iops, ofs_link_iops;
static const struct file_operations ofs_file_fops, ofs_dir_fops;
static const struct address_space_operations ofs_aops;
static const struct dentry_operations ofs_dentry_ops;
static const struct xattr_handler * const ofs_xattrs[];
static struct ofs_mount *ofs_mount(struct super_block *sb) { return sb->s_fs_info; }
static struct ofs_inode *ofs_inode(struct inode *inode) {
    return container_of(inode, struct ofs_inode, inode);
}
static int status(int r) {
    switch (r) {
    case 0: return 0;
    case IFS_OFS_END: case IFS_OFS_NOT_FOUND: return -ENOENT;
    case IFS_OFS_INVALID: return -EINVAL;
    case IFS_OFS_CORRUPT: return -EUCLEAN;
    case IFS_OFS_NO_SPACE: return -ENOSPC;
    case IFS_OFS_EXISTS: return -EEXIST;
    case IFS_OFS_ACCESS: return -EACCES;
    case IFS_OFS_NO_MEMORY: return -ENOMEM;
    default: return -EIO;
    }
}
static int byte_io(struct ofs_mount *m, u64 at, void *data, u32 count, bool write) {
    u32 unit = m->sb->s_blocksize, done = 0;
    u64 bytes = bdev_nr_bytes(m->sb->s_bdev);
    if (at > bytes || count > bytes - at || (write && sb_rdonly(m->sb))) return -1;
    while (done < count) {
        u64 offset = at + done;
        u32 within = offset & (unit - 1u), take = min(count - done, unit - within);
        struct buffer_head *bh = sb_bread(m->sb, offset >> m->sb->s_blocksize_bits);
        if (!bh) { m->error = -EIO; return -1; }
        if (write) {
            lock_buffer(bh);
            memcpy(bh->b_data + within, (u8 *)data + done, take);
            set_buffer_uptodate(bh);
            unlock_buffer(bh);
            mark_buffer_dirty(bh);
        } else {
            lock_buffer(bh);
            memcpy((u8 *)data + done, bh->b_data + within, take);
            unlock_buffer(bh);
        }
        brelse(bh); done += take;
    }
    return 0;
}
static int read_io(void *ctx, u64 at, void *p, u32 n) { return byte_io(ctx, at, p, n, false); }
static int write_io(void *ctx, u64 at, const void *p, u32 n) {
    return byte_io(ctx, at, (void *)p, n, true);
}
static int flush_io(void *ctx) {
    struct ofs_mount *m = ctx;
    int r = sync_blockdev(m->sb->s_bdev);
    if (r) m->error = r;
    return r || m->error ? -1 : 0;
}
static void *allocate_io(void *ctx, size_t n) { (void)ctx; return kvmalloc(n, GFP_NOFS); }
static void release_io(void *ctx, void *p) { (void)ctx; kvfree(p); }
static int clock_io(void *ctx, IfsOfsDate *out) {
    s64 seconds = ktime_get_real_seconds() - OFS_EPOCH - (s64)sys_tz.tz_minuteswest * 60;
    s32 remain;
    (void)ctx;
    if (seconds < 0) seconds = 0;
    out->days = div_s64_rem(seconds, 86400, &remain);
    out->minutes = remain / 60; out->ticks = (remain % 60) * 50;
    return 0;
}
static struct timespec64 timestamp(IfsOfsDate d) {
    struct timespec64 t;
    t.tv_sec = OFS_EPOCH + (s64)sys_tz.tz_minuteswest * 60 +
        (s64)d.days * 86400 + (s64)d.minutes * 60 + d.ticks / 50;
    t.tv_nsec = (d.ticks % 50) * 20000000L;
    return t;
}
static IfsOfsDate datestamp(struct timespec64 t) {
    s64 seconds = t.tv_sec - OFS_EPOCH - (s64)sys_tz.tz_minuteswest * 60;
    s32 remain;
    IfsOfsDate d = {0};
    if (seconds < 0) return d;
    d.days = div_s64_rem(seconds, 86400, &remain);
    d.minutes = remain / 60; d.ticks = (remain % 60) * 50 + t.tv_nsec / 20000000L;
    return d;
}
static umode_t protection_mode(u32 p) {
    umode_t mode = 0;
    if (!(p & 8)) mode |= 0400;
    if (!(p & 4)) mode |= 0200;
    if (!(p & 2)) mode |= 0100;
    if (p & 0x800) mode |= 0040;
    if (p & 0x400) mode |= 0020;
    if (p & 0x200) mode |= 0010;
    if (p & 0x8000) mode |= 0004;
    if (p & 0x4000) mode |= 0002;
    if (p & 0x2000) mode |= 0001;
    return mode;
}
static u32 mode_protection(u32 p, umode_t mode) {
    p &= ~0xff0fU;
    if (!(mode & 0400)) p |= 8;
    if (!(mode & 0200)) p |= 4;
    if (!(mode & 0100)) p |= 2;
    if (mode & 0040) p |= 0x800;
    if (mode & 0020) p |= 0x400;
    if (mode & 0010) p |= 0x200;
    if (mode & 0070) p |= 0x100;
    if (mode & 0004) p |= 0x8000;
    if (mode & 0002) p |= 0x4000;
    if (mode & 0001) p |= 0x2000;
    if (mode & 0007) p |= 0x1000;
    return p;
}
static int link_text(struct ofs_mount *m, u32 key, char **out, u32 *bytes) {
    OfsFs *f = m->fs; u8 *block = f->scratch[2];
    int r = ofs_metadata(f, key, block);
    if (r) return r;
    u32 count = 0, limit = f->block_bytes - 224u;
    while (count < limit && block[24 + count]) ++count;
    if (count == limit) return IFS_OFS_CORRUPT;
    r = ofs_utf8(block + 24, count, f->link_path[0], sizeof(f->link_path[0]));
    const char *prefix = m->prefix ? m->prefix : "/";
    if (!r) r = ifs_ofs_translate_symlink((u8 *)f->link_path[0], sizeof(f->link_path[0]),
              (const u8 *)prefix, strlen(prefix), (u8 *)f->link_path[1], sizeof(f->link_path[1]), bytes);
    if (r) return IFS_OFS_CORRUPT;
    *out = f->link_path[1]; return 0;
}
static int read_node(struct inode *inode, OfsNode *n) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    return ofs_node(m->fs, ofs_inode(inode)->object, n, m->fs->scratch[0]);
}
static int refresh(struct inode *inode, const OfsNode *n) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    struct timespec64 t = timestamp(n->changed);
    umode_t type = n->object_type == IFS_OFS_FILE ? S_IFREG :
        n->object_type == IFS_OFS_SOFTLINK ? S_IFLNK : S_IFDIR;
    ofs_inode(inode)->object = n->object; ofs_inode(inode)->protection = n->protection;
    inode->i_mode = type | (m->force_mode ? m->mode : n->type == IFS_OFS_ROOT ? 0755 : protection_mode(n->protection));
    i_uid_write(inode, !n->uid || m->force_uid ? m->uid : m->mufs && n->uid == U16_MAX ? 0 : n->uid);
    i_gid_write(inode, !n->gid || m->force_gid ? m->gid : m->mufs && n->gid == U16_MAX ? 0 : n->gid);
    inode->i_flags = m->protect ? inode->i_flags | S_IMMUTABLE : inode->i_flags & ~S_IMMUTABLE;
    inode_set_mtime_to_ts(inode, t); inode_set_ctime_to_ts(inode, t); inode_set_atime_to_ts(inode, t);
    if (S_ISREG(type)) i_size_write(inode, n->bytes);
    if (S_ISLNK(type)) {
        char *text; u32 bytes;
        int r = link_text(m, n->key, &text, &bytes);
        if (r) return r;
        i_size_write(inode, bytes);
    }
    u64 blocks = DIV_ROUND_UP_ULL(n->bytes, m->fs->block_bytes - 24u);
    inode->i_blocks = (blocks + 1u + (blocks ? (blocks - 1u) / m->fs->hash_size : 0)) *
                      (m->fs->block_bytes / 512u);
    u32 links = ofs_is_orphan(m->fs, n->object) ? 0 : 1, key = n->link, seen = 0;
    while (key) {
        OfsNode alias;
        if (++seen > m->fs->blocks - m->fs->reserved) return IFS_OFS_CORRUPT;
        int r = ofs_node(m->fs, key, &alias, m->fs->scratch[0]);
        if (r || alias.object != n->object) return r ? r : IFS_OFS_CORRUPT;
        ++links; key = alias.link;
    }
    set_nlink(inode, S_ISDIR(type) && IS_DEADDIR(inode) ? 0 : S_ISDIR(type) && links ? 2 : links);
    return 0;
}
static struct inode *get_inode(struct super_block *sb, const OfsNode *n) {
    /* Linux directory aliases have separate dentries/inodes while their VFS
     * operations address the same native object. Regular hard links share the
     * original inode, including their open/unlink and page-cache lifetime. */
    u64 number = n->type == IFS_OFS_LINKDIR ? ((u64)n->object << 32) | n->key : n->object;
    struct inode *inode = iget_locked(sb, number);
    if (!inode) return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW)) return inode;
    struct ofs_mount *m = ofs_mount(sb);
    mutex_lock(&m->lock);
    OfsNode fresh;
    int r = ofs_node(m->fs, n->object, &fresh, m->fs->scratch[0]);
    if (!r) r = refresh(inode, &fresh);
    mutex_unlock(&m->lock);
    if (r) { iget_failed(inode); return ERR_PTR(status(r)); }
    inode->i_mapping->a_ops = &ofs_aops;
    mapping_set_folio_order_range(inode->i_mapping, 0, get_order(65536));
    if (n->object_type == IFS_OFS_FILE) {
        inode->i_op = &ofs_file_iops; inode->i_fop = &ofs_file_fops;
    } else if (n->object_type == IFS_OFS_SOFTLINK) inode->i_op = &ofs_link_iops;
    else { inode->i_op = &ofs_dir_iops; inode->i_fop = &ofs_dir_fops; }
    unlock_new_inode(inode); return inode;
}
static int name_bytes(const struct qstr *name, u8 out[108], u32 *length) {
    char utf8[321];
    if (!name->len || name->len >= sizeof(utf8)) return -ENAMETOOLONG;
    memcpy(utf8, name->name, name->len); utf8[name->len] = 0;
    return status(ofs_latin1(utf8, out, 107, length, 1));
}
static int hash_dentry(const struct dentry *parent, struct qstr *name) {
    struct ofs_mount *m = ofs_mount(parent->d_sb);
    u8 folded[108]; u32 length;
    int r = name_bytes(name, folded, &length);
    if (r) return r;
    for (u32 i = 0; i < length; ++i)
        folded[i] = ifs_ofs_fold_character(folded[i], m->fs->variant != 0);
    name->hash = full_name_hash(parent, folded, length);
    return 0;
}
static int compare_dentry(const struct dentry *dentry, unsigned len, const char *name, const struct qstr *other) {
    struct ofs_mount *m = ofs_mount(dentry->d_sb);
    struct qstr first = QSTR_INIT(name, len);
    u8 a[108], b[108]; u32 an, bn;
    if (name_bytes(&first, a, &an) || name_bytes(other, b, &bn)) return 1;
    return !ofs_name_equal(m->fs, a, an, b, bn);
}
static int revalidate_dentry(struct dentry *dentry, unsigned flags) {
    if (IS_ROOT(dentry)) return 1;
    if (flags & LOOKUP_RCU) return -ECHILD;
    if (d_really_is_negative(dentry)) return 0;
    struct inode *parent = d_inode(dentry->d_parent);
    if (!parent) return 0;
    struct ofs_mount *m = ofs_mount(dentry->d_sb);
    u8 name[108]; u32 length;
    if (name_bytes(&dentry->d_name, name, &length)) return 0;
    mutex_lock(&m->lock);
    OfsNode n;
    int r = ofs_lookup(m->fs, ofs_inode(parent)->object, name, length, &n);
    int valid = !r && n.object == ofs_inode(d_inode(dentry))->object;
    mutex_unlock(&m->lock);
    return r && r != IFS_OFS_NOT_FOUND ? status(r) : valid;
}
static const struct dentry_operations ofs_dentry_ops = {
    .d_hash = hash_dentry, .d_compare = compare_dentry, .d_revalidate = revalidate_dentry,
};
static struct dentry *lookup(struct inode *dir, struct dentry *dentry, unsigned flags) {
    struct ofs_mount *m = ofs_mount(dir->i_sb);
    u8 name[108]; u32 length;
    int r = name_bytes(&dentry->d_name, name, &length);
    struct inode *inode = NULL;
    (void)flags;
    if (r) return ERR_PTR(r);
    mutex_lock(&m->lock);
    OfsNode n;
    r = ofs_lookup(m->fs, ofs_inode(dir)->object, name, length, &n);
    mutex_unlock(&m->lock);
    if (r && r != IFS_OFS_NOT_FOUND) return ERR_PTR(status(r));
    if (!r) inode = get_inode(dir->i_sb, &n);
    return d_splice_alias(inode, dentry);
}
static char *path_of(struct dentry *dentry, char **allocation) {
    *allocation = kmalloc(PATH_MAX, GFP_NOFS);
    return *allocation ? dentry_path_raw(dentry, *allocation, PATH_MAX) : ERR_PTR(-ENOMEM);
}
static int iterate(struct file *file, struct dir_context *ctx) {
    struct inode *inode = file_inode(file);
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    if (!dir_emit_dots(file, ctx)) return 0;
    mutex_lock(&m->lock);
    OfsDirCursor cursor = {0}; OfsNode n; u64 index = 2;
    int r;
    while (!(r = ofs_next(m->fs, ofs_inode(inode)->object, &cursor, &n))) {
        if (index++ < ctx->pos) continue;
        char name[321];
        r = ofs_utf8(n.name, n.name_bytes, name, sizeof(name));
        if (r) break;
        unsigned type = n.object_type == IFS_OFS_FILE ? DT_REG :
                        n.object_type == IFS_OFS_SOFTLINK ? DT_LNK : DT_DIR;
        u64 number = n.type == IFS_OFS_LINKDIR ? ((u64)n.object << 32) | n.key : n.object;
        if (!dir_emit(ctx, name, strlen(name), number, type)) { r = 0; break; }
        ++ctx->pos;
    }
    mutex_unlock(&m->lock);
    return r == IFS_OFS_END ? 0 : status(r);
}
static int initialise(struct inode *dir, struct dentry *dentry, umode_t mode, s32 type, const char *target) {
    struct ofs_mount *m = ofs_mount(dir->i_sb);
    char *memory, *path = path_of(dentry, &memory);
    int r = IS_ERR(path) ? PTR_ERR(path) : 0;
    if (from_kuid(&init_user_ns, current_fsuid()) > U16_MAX ||
        from_kgid(&init_user_ns, current_fsgid()) > U16_MAX) r = -EOVERFLOW;
    if (r) { kfree(memory); return r; }
    mutex_lock(&m->lock);
    r = ofs_create(m->fs, path, type, target, 0);
    OfsNode n;
    if (!r) r = ofs_path(m->fs, path, 0, &n);
    if (!r && type != IFS_OFS_SOFTLINK) {
        u32 protection = mode_protection(n.protection, mode);
        r = ofs_set_metadata_key(m->fs, n.object, 1, &protection);
    }
    if (!r) {
        u16 ids[2] = {from_kuid(&init_user_ns, current_fsuid()), from_kgid(&init_user_ns, current_fsgid())};
        r = ofs_set_metadata_key(m->fs, n.object, 2, ids);
    }
    if (!r) r = ofs_path(m->fs, path, 0, &n);
    mutex_unlock(&m->lock); kfree(memory);
    if (r) return status(r);
    struct inode *inode = get_inode(dir->i_sb, &n);
    if (IS_ERR(inode)) return PTR_ERR(inode);
    d_instantiate(dentry, inode); inode_inc_iversion(dir);
    return 0;
}
static int create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    (void)idmap; (void)excl; return initialise(dir, dentry, mode, IFS_OFS_FILE, NULL);
}
static IFS_LINUX_MKDIR_RETURN mkdir_inode(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
    (void)idmap;
    int r = initialise(dir, dentry, mode, IFS_OFS_DIRECTORY, NULL);
    if (r) return IFS_LINUX_MKDIR_FAILURE(r);
    return IFS_LINUX_MKDIR_SUCCESS;
}
static int unlink_inode(struct inode *dir, struct dentry *dentry) {
    struct ofs_mount *m = ofs_mount(dir->i_sb);
    char *memory, *path = path_of(dentry, &memory);
    if (IS_ERR(path)) { int r = PTR_ERR(path); kfree(memory); return r; }
    struct inode *inode = d_inode(dentry);
    mutex_lock(&m->lock);
    int r = ofs_unlink_deferred(m->fs, path);
    if (!r) {
        if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)) clear_nlink(inode);
        else {
            OfsNode n;
            r = read_node(inode, &n);
            if (!r) r = refresh(inode, &n);
        }
    }
    mutex_unlock(&m->lock); kfree(memory);
    if (!r) inode_inc_iversion(dir);
    return status(r);
}
static int link_inode(struct dentry *old, struct inode *dir, struct dentry *dentry) {
    struct inode *inode = d_inode(old);
    struct ofs_mount *m = ofs_mount(dir->i_sb);
    char *memory, *path = path_of(dentry, &memory);
    if (IS_ERR(path)) { int r = PTR_ERR(path); kfree(memory); return r; }
    mutex_lock(&m->lock);
    int r = ofs_create(m->fs, path, IFS_OFS_LINKFILE, NULL, ofs_inode(inode)->object);
    OfsNode n;
    if (!r) r = read_node(inode, &n);
    if (!r) r = refresh(inode, &n);
    mutex_unlock(&m->lock); kfree(memory);
    if (!r) { ihold(inode); d_instantiate(dentry, inode); inode_inc_iversion(dir); }
    return status(r);
}
static int rename_inode(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old,
                         struct inode *new_dir, struct dentry *to, unsigned flags) {
    struct ofs_mount *m = ofs_mount(old_dir->i_sb);
    char *a = NULL, *b = NULL, *from = path_of(old, &a), *dest = path_of(to, &b);
    int r = IS_ERR(from) ? PTR_ERR(from) : IS_ERR(dest) ? PTR_ERR(dest) : 0;
    (void)idmap;
    if (flags & ~RENAME_NOREPLACE) r = -EINVAL;
    if (!r && flags && d_really_is_positive(to)) r = -EEXIST;
    if (r) { kfree(a); kfree(b); return r; }
    mutex_lock(&m->lock);
    r = ofs_rename_deferred(m->fs, from, dest);
    if (!r && d_really_is_positive(to)) {
        if (S_ISDIR(d_inode(to)->i_mode)) clear_nlink(d_inode(to));
        else drop_nlink(d_inode(to));
    }
    mutex_unlock(&m->lock); kfree(a); kfree(b);
    if (!r) { inode_inc_iversion(old_dir); inode_inc_iversion(new_dir); }
    return status(r);
}
static int symlink_inode(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *target) {
    /* POSIX's '/' and '../' need translation to native Amiga target syntax.
     * An initial ':' names this same volume, independent of mount location. */
    size_t bytes = strlen(target);
    char *native = kmalloc(bytes + 2, GFP_NOFS);
    if (!native) return -ENOMEM;
    u32 length = 0;
    const char *volume = ofs_mount(dir->i_sb)->volume;
    int r = ifs_ofs_encode_symlink((const u8 *)target, bytes + 1, (const u8 *)volume, strlen(volume),
                                   (u8 *)native, bytes + 2, &length);
    (void)idmap;
    r = r ? -EINVAL : initialise(dir, dentry, 0777, IFS_OFS_SOFTLINK, native);
    kfree(native); return r;
}
static const char *get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    if (!dentry) return ERR_PTR(-ECHILD);
    mutex_lock(&m->lock);
    char *text = NULL, *copy = NULL; u32 bytes;
    int r = link_text(m, ofs_inode(inode)->object, &text, &bytes);
    if (!r) {
        copy = kvmalloc(bytes + 1u, GFP_NOFS);
        if (!copy) r = IFS_OFS_NO_MEMORY;
        else memcpy(copy, text, bytes + 1u);
    }
    mutex_unlock(&m->lock);
    if (r) { kvfree(copy); return ERR_PTR(status(r)); }
    set_delayed_call(done, (void (*)(void *))kvfree, copy); return copy;
}
static int fill_folio(struct inode *inode, struct folio *folio) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    u64 at = folio_pos(folio);
    mutex_lock(&m->lock);
    OfsNode n;
    int r = read_node(inode, &n);
    u32 count = !r && at < n.bytes ? min_t(u64, folio_size(folio), n.bytes - at) : 0;
    void *data = count ? kvmalloc(count, GFP_NOFS) : NULL;
    if (!r && count && !data) r = IFS_OFS_NO_MEMORY;
    if (!r && count) r = ofs_read_file(m->fs, &n, at, data, count, NULL);
    if (!r) {
        if (count) memcpy_to_folio(folio, 0, data, count);
        folio_zero_range(folio, count, folio_size(folio) - count);
    }
    kvfree(data); mutex_unlock(&m->lock);
    if (!r) { flush_dcache_folio(folio); folio_mark_uptodate(folio); }
    return status(r);
}
static int read_folio(struct file *file, struct folio *folio) {
    (void)file;
    int r = fill_folio(folio->mapping->host, folio);
    folio_unlock(folio); return r;
}
static int write_begin(IFS_LINUX_AOPS_WRITE_CONTEXT, struct address_space *mapping,
                         loff_t at, unsigned len, struct folio **out, void **data) {
    (void)IFS_LINUX_AOPS_WRITE_CONTEXT_ARG; (void)data;
    if (at < 0 || (u64)at + len > U32_MAX) return -EFBIG;
    struct folio *folio = __filemap_get_folio(mapping, at >> PAGE_SHIFT, FGP_WRITEBEGIN | fgf_set_order(len),
                                             mapping_gfp_mask(mapping));
    if (IS_ERR(folio)) return PTR_ERR(folio);
    int r = folio_test_uptodate(folio) ? 0 : fill_folio(mapping->host, folio);
    if (r) { folio_unlock(folio); folio_put(folio); return r; }
    *out = folio; return 0;
}
static int commit_bytes(struct inode *inode, u64 at, const void *data, u32 bytes) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    if (at > U32_MAX || bytes > U32_MAX - at) return -EFBIG;
    mutex_lock(&m->lock);
    OfsNode n;
    int r = read_node(inode, &n);
    if (!r) r = ofs_write_checked(m->fs, &n, at, data, bytes);
    if (!r) r = read_node(inode, &n);
    if (!r) r = refresh(inode, &n);
    mutex_unlock(&m->lock);
    return status(r);
}
static int write_end(IFS_LINUX_AOPS_WRITE_CONTEXT, struct address_space *mapping,
                       loff_t at, unsigned len, unsigned copied, struct folio *folio, void *opaque) {
    (void)IFS_LINUX_AOPS_WRITE_CONTEXT_ARG; (void)opaque;
    copied = min(copied, len);
    void *data = copied ? kvmalloc(copied, GFP_NOFS) : NULL;
    int r = copied && !data ? -ENOMEM : 0;
    if (!r && copied) {
        memcpy_from_folio(data, folio, offset_in_folio(folio, at), copied);
        r = commit_bytes(mapping->host, at, data, copied);
    }
    kvfree(data);
    if (r) { folio_clear_uptodate(folio); mapping_set_error(mapping, r); }
    folio_unlock(folio); folio_put(folio);
    return r ? r : copied;
}
static int write_folio(struct folio *folio, struct writeback_control *wbc, void *opaque) {
    struct inode *inode = folio->mapping->host;
    u64 at = folio_pos(folio), size = i_size_read(inode);
    u32 count = at < size ? min_t(u64, folio_size(folio), size - at) : 0;
    (void)wbc; (void)opaque;
    folio_start_writeback(folio);
    void *data = count ? kvmalloc(count, GFP_NOFS) : NULL;
    int r = count && !data ? -ENOMEM : 0;
    if (!r && count) { memcpy_from_folio(data, folio, 0, count); r = commit_bytes(inode, at, data, count); }
    kvfree(data);
    if (r) { mapping_set_error(inode->i_mapping, r); folio_redirty_for_writepage(wbc, folio); }
    folio_end_writeback(folio); folio_unlock(folio); return r;
}
static int writepages(struct address_space *mapping, struct writeback_control *wbc) {
    return write_cache_pages(mapping, wbc, write_folio, NULL);
}
static int fsync_file(struct file *file, loff_t start, loff_t end, int datasync) {
    int r = file_write_and_wait_range(file, start, end);
    struct ofs_mount *m = ofs_mount(file_inode(file)->i_sb);
    (void)datasync;
    if (r) return r;
    mutex_lock(&m->lock); r = flush_io(m) ? -EIO : 0; mutex_unlock(&m->lock);
    return r;
}
static int getattr_inode(struct mnt_idmap *idmap, const struct path *path,
                           struct kstat *stat, u32 mask, unsigned flags) {
    struct inode *inode = d_inode(path->dentry);
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    mutex_lock(&m->lock);
    OfsNode n;
    int r = read_node(inode, &n);
    if (!r) r = refresh(inode, &n);
    if (!r) {
        generic_fillattr(idmap, mask, inode, stat);
        stat->blksize = m->fs->block_bytes;
        if (n.type == IFS_OFS_ROOT) { stat->btime = timestamp(n.created); stat->result_mask |= STATX_BTIME; }
    }
    mutex_unlock(&m->lock); (void)flags;
    return status(r);
}
static int permission_inode(struct mnt_idmap *idmap, struct inode *inode, int mask) {
    /* Native directory aliases have separate Linux inodes. Refresh before
     * permission checks so chmod/chown through another alias cannot leave a
     * stale cached permission grant. RCU lookup retries in blocking mode. */
    if (mask & MAY_NOT_BLOCK) return -ECHILD;
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    mutex_lock(&m->lock);
    OfsNode n;
    int r = read_node(inode, &n);
    if (!r) r = refresh(inode, &n);
    mutex_unlock(&m->lock);
    return r ? status(r) : generic_permission(idmap, inode, mask);
}
static int setattr_inode(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr) {
    struct inode *inode = d_inode(dentry);
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    int r = setattr_prepare(idmap, dentry, attr);
    if (r) return r;
    if (m->protect || ((attr->ia_valid & ATTR_MODE) && m->force_mode) ||
        ((attr->ia_valid & ATTR_UID) && m->force_uid) ||
        ((attr->ia_valid & ATTR_GID) && m->force_gid)) return -EPERM;
    if ((attr->ia_valid & ATTR_SIZE) && (attr->ia_size < 0 || attr->ia_size > U32_MAX)) return -EFBIG;
    if ((attr->ia_valid & ATTR_SIZE) && (r = filemap_write_and_wait(inode->i_mapping))) return r;
    if ((attr->ia_valid & ATTR_UID) && from_kuid(&init_user_ns, attr->ia_uid) > U16_MAX) return -EOVERFLOW;
    if ((attr->ia_valid & ATTR_GID) && from_kgid(&init_user_ns, attr->ia_gid) > U16_MAX) return -EOVERFLOW;
    mutex_lock(&m->lock);
    OfsNode n;
    r = read_node(inode, &n);
    if (!r && (attr->ia_valid & ATTR_SIZE)) r = ofs_resize_checked(m->fs, &n, attr->ia_size);
    if (!r && (attr->ia_valid & ATTR_MODE)) {
        u32 p = mode_protection(n.protection, attr->ia_mode);
        r = ofs_set_metadata_key(m->fs, n.object, 1, &p);
    }
    if (!r && (attr->ia_valid & (ATTR_UID | ATTR_GID))) {
        u16 ids[2] = {n.uid, n.gid};
        if (attr->ia_valid & ATTR_UID) ids[0] = from_kuid(&init_user_ns, attr->ia_uid);
        if (attr->ia_valid & ATTR_GID) ids[1] = from_kgid(&init_user_ns, attr->ia_gid);
        r = ofs_set_metadata_key(m->fs, n.object, 2, ids);
    }
    if (!r && (attr->ia_valid & ATTR_MTIME)) {
        IfsOfsDate date = datestamp(attr->ia_mtime);
        r = ofs_set_metadata_key(m->fs, n.object, 4u | (1u << 8), &date);
    }
    if (!r) r = read_node(inode, &n);
    if (!r) r = refresh(inode, &n);
    mutex_unlock(&m->lock);
    if (!r && (attr->ia_valid & ATTR_SIZE)) truncate_setsize(inode, attr->ia_size);
    return status(r);
}
static int xattr_get(const struct xattr_handler *h, struct dentry *dentry, struct inode *inode,
                     const char *name, void *data, size_t cap) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    (void)dentry; (void)name;
    mutex_lock(&m->lock);
    OfsNode n;
    int r = read_node(inode, &n);
    char text[321]; size_t bytes = 0;
    if (!r && h->flags == 1) { r = ofs_utf8(n.comment, n.comment_bytes, text, sizeof(text)); bytes = r ? 0 : strlen(text); }
    if (!r && h->flags == 2) { r = ofs_utf8(n.name, n.name_bytes, text, sizeof(text)); bytes = r ? 0 : strlen(text); }
    if (!r && h->flags == 3) { ofs_put32(text, n.protection); bytes = 4; }
    if (!r && h->flags == 4) {
        if (n.type != IFS_OFS_ROOT) r = IFS_OFS_INVALID;
        else { ofs_put_date((u8 *)text, &n.created); bytes = 12; }
    }
    mutex_unlock(&m->lock);
    if (r) return status(r);
    if (cap && cap < bytes) return -ERANGE;
    if (cap) memcpy(data, text, bytes);
    return bytes;
}
static int xattr_set(const struct xattr_handler *h, struct mnt_idmap *idmap,
                     struct dentry *dentry, struct inode *inode, const char *name,
                     const void *data, size_t size, int flags) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    (void)idmap; (void)dentry; (void)name;
    if (flags & XATTR_CREATE) return -EEXIST;
    if (m->protect) return -EPERM;
    if (sb_rdonly(inode->i_sb)) return -EROFS;
    char *text = NULL;
    u32 protection = 0; IfsOfsDate date;
    if (h->flags <= 2) {
        if (size > 320 || (size && (!data || memchr(data, 0, size)))) return -EINVAL;
        text = kmemdup_nul(data ? data : "", size, GFP_NOFS);
        if (!text) return -ENOMEM;
    } else if (h->flags == 3) {
        if (!data || size != 4) return -EINVAL;
        protection = ofs_be32(data);
    } else {
        if (!data || size != 12) return -EINVAL;
        date = ofs_get_date(data);
    }
    mutex_lock(&m->lock);
    int r = h->flags == 1 ? ofs_set_metadata_key(m->fs, ofs_inode(inode)->object, 3, text) :
            h->flags == 2 ? ofs_inode(inode)->object == m->fs->root ? ofs_set_label(m->fs, text) : IFS_OFS_INVALID :
            h->flags == 3 ? ofs_set_metadata_key(m->fs, ofs_inode(inode)->object, 1, &protection) :
            ofs_set_metadata_key(m->fs, ofs_inode(inode)->object, 4u | (4u << 8), &date);
    OfsNode n;
    if (!r) r = read_node(inode, &n);
    if (!r) r = refresh(inode, &n);
    mutex_unlock(&m->lock); kfree(text); return status(r);
}
static const struct xattr_handler comment_xattr = {.name = "user.amiga.comment", .flags = 1, .get = xattr_get, .set = xattr_set};
static const struct xattr_handler label_xattr = {.name = "user.amiga.label", .flags = 2, .get = xattr_get, .set = xattr_set};
static const struct xattr_handler protection_xattr = {.name = "user.amiga.protection", .flags = 3, .get = xattr_get, .set = xattr_set};
static const struct xattr_handler created_xattr = {.name = "user.amiga.created", .flags = 4, .get = xattr_get, .set = xattr_set};
static const struct xattr_handler * const ofs_xattrs[] = {&comment_xattr, &label_xattr, &protection_xattr, &created_xattr, NULL};
static ssize_t listxattr(struct dentry *dentry, char *out, size_t cap) {
    const char file[] = "user.amiga.comment\0user.amiga.protection";
    const char root[] = "user.amiga.label\0user.amiga.created";
    bool is_root = ofs_inode(d_inode(dentry))->object == ofs_mount(dentry->d_sb)->fs->root;
    const char *names = is_root ? root : file;
    size_t bytes = is_root ? sizeof(root) : sizeof(file);
    if (cap && cap < bytes) return -ERANGE;
    if (cap) memcpy(out, names, bytes);
    return bytes;
}
static struct inode *allocate_inode(struct super_block *sb) {
    struct ofs_inode *node = kzalloc(sizeof(*node), GFP_KERNEL);
    (void)sb;
    if (!node) return NULL;
    inode_init_once(&node->inode); return &node->inode;
}
static void free_inode(struct inode *inode) { kfree(ofs_inode(inode)); }
static void evict_inode(struct inode *inode) {
    struct ofs_mount *m = ofs_mount(inode->i_sb);
    truncate_inode_pages_final(inode->i_mapping);
    if (!inode->i_nlink && !is_bad_inode(inode) && m) {
        mutex_lock(&m->lock);
        if (ofs_is_orphan(m->fs, ofs_inode(inode)->object)) {
            int r = ofs_release_orphan(m->fs, ofs_inode(inode)->object);
            if (r) m->error = status(r);
        }
        mutex_unlock(&m->lock);
    }
    clear_inode(inode);
}
static int sync_fs(struct super_block *sb, int wait) {
    struct ofs_mount *m = ofs_mount(sb);
    int r;
    (void)wait;
    mutex_lock(&m->lock); r = flush_io(m) ? -EIO : 0; mutex_unlock(&m->lock);
    return r;
}
static void put_super(struct super_block *sb) {
    struct ofs_mount *m = ofs_mount(sb);
    if (!m) return;
    (void)flush_io(m); ofs_release(m->fs); kvfree(m->fs); kfree(m->prefix); kfree(m); sb->s_fs_info = NULL;
}
static int statfs_inode(struct dentry *dentry, struct kstatfs *out) {
    struct ofs_mount *m = ofs_mount(dentry->d_sb);
    IfsOfsSpaceV3 space;
    mutex_lock(&m->lock); int r = ofs_space(m->fs, &space); mutex_unlock(&m->lock);
    if (r) return status(r);
    out->f_type = AFFS_SUPER_MAGIC; out->f_bsize = m->fs->block_bytes;
    out->f_blocks = space.total_bytes / m->fs->block_bytes;
    out->f_bfree = out->f_bavail = space.free_bytes / m->fs->block_bytes;
    out->f_namelen = m->fs->variant == 6 ? 106 : 30;
    return 0;
}
static int show_options(struct seq_file *seq, struct dentry *root) {
    struct ofs_mount *m = ofs_mount(root->d_sb);
    seq_printf(seq, ",bs=%u,reserved=%u,root=%u", m->fs->block_bytes, m->fs->reserved, m->fs->root);
    if (m->force_uid) seq_printf(seq, ",setuid=%u", m->uid);
    if (m->force_gid) seq_printf(seq, ",setgid=%u", m->gid);
    if (m->force_mode) seq_printf(seq, ",mode=%o", m->mode);
    if (m->protect) seq_puts(seq, ",protect");
    if (m->mufs) seq_puts(seq, ",mufs");
    if (m->verbose) seq_puts(seq, ",verbose");
    if (m->prefix) seq_show_option(seq, "prefix", m->prefix);
    if (strcmp(m->volume, ":")) seq_show_option(seq, "volume", m->volume);
    return 0;
}
static int remount(struct super_block *sb, int *flags, char *data) {
    struct ofs_mount *m = ofs_mount(sb);
    /* Presentation and geometry are chosen at mount time. Do not silently
     * accept changes that would leave already cached inodes inconsistent. */
    if (data && *data) return -EINVAL;
    int synced = sync_filesystem(sb);
    if (synced) return synced;
    mutex_lock(&m->lock);
    int r = 0;
    if (*flags & SB_RDONLY) m->fs->io.flags |= IFS_OFS_READ_ONLY;
    else if (m->fs->io.flags & IFS_OFS_READ_ONLY) {
        m->fs->io.flags &= ~IFS_OFS_READ_ONLY;
        IfsOfsCheckV3 check;
        r = ofs_verify(m->fs, &check, 0);
        if (r) m->fs->io.flags |= IFS_OFS_READ_ONLY;
        else m->fs->used_blocks = check.referenced_blocks;
    }
    mutex_unlock(&m->lock); return status(r);
}
static const struct super_operations ofs_super_ops = {
    .alloc_inode = allocate_inode, .free_inode = free_inode, .evict_inode = evict_inode,
    .put_super = put_super, .sync_fs = sync_fs, .statfs = statfs_inode,
    .show_options = show_options, .remount_fs = remount,
};
static const struct address_space_operations ofs_aops = {
    .read_folio = read_folio, .write_begin = write_begin, .write_end = write_end,
    .dirty_folio = filemap_dirty_folio, .writepages = writepages,
};
static const struct file_operations ofs_file_fops = {
    .llseek = generic_file_llseek, .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter, .mmap = generic_file_mmap,
    .fsync = fsync_file, .splice_read = filemap_splice_read,
};
static const struct file_operations ofs_dir_fops = {
    .llseek = generic_file_llseek, .read = generic_read_dir, .iterate_shared = iterate, .fsync = fsync_file,
};
static const struct inode_operations ofs_file_iops = {
    .permission = permission_inode, .setattr = setattr_inode, .getattr = getattr_inode, .listxattr = listxattr,
};
static const struct inode_operations ofs_dir_iops = {
    .lookup = lookup, .create = create, .mkdir = mkdir_inode, .unlink = unlink_inode,
    .rmdir = unlink_inode, .link = link_inode, .symlink = symlink_inode,
    .rename = rename_inode, .permission = permission_inode, .setattr = setattr_inode, .getattr = getattr_inode, .listxattr = listxattr,
};
static const struct inode_operations ofs_link_iops = {
    .get_link = get_link, .permission = permission_inode, .setattr = setattr_inode, .getattr = getattr_inode, .listxattr = listxattr,
};
static int fill_super(struct super_block *sb, void *data, int silent) {
    struct ofs_mount *m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m) return -ENOMEM;
    m->fs = kvzalloc(sizeof(*m->fs), GFP_KERNEL);
    if (!m->fs) { kfree(m); return -ENOMEM; }
    m->sb = sb; m->uid = from_kuid(&init_user_ns, current_uid());
    m->gid = from_kgid(&init_user_ns, current_gid()); strcpy(m->volume, ":");
    mutex_init(&m->lock); sb->s_fs_info = m;
    sb->s_op = &ofs_super_ops; sb->s_xattr = ofs_xattrs; sb->s_d_op = &ofs_dentry_ops;
    sb->s_magic = AFFS_SUPER_MAGIC; sb->s_maxbytes = U32_MAX;
    sb->s_time_gran = 20000000;
    (void)silent;
    int r = -EINVAL;
    if (!sb_min_blocksize(sb, bdev_logical_block_size(sb->s_bdev))) goto fail;
    IfsOfsMountV3 options = {.structure_bytes = sizeof(options)};
    char *settings = kstrdup(data ? data : "", GFP_KERNEL), *cursor = settings, *token;
    if (!settings) { r = -ENOMEM; goto fail; }
    while ((token = strsep(&cursor, ","))) {
        u32 *value;
        if (!*token) continue;
        if (!strncmp(token, "setuid=", 7)) {
            if (kstrtou32(token + 7, 10, &m->uid) || !uid_valid(make_kuid(&init_user_ns, m->uid))) { kfree(settings); goto fail; }
            m->force_uid = true; continue;
        }
        if (!strncmp(token, "setgid=", 7)) {
            if (kstrtou32(token + 7, 10, &m->gid) || !gid_valid(make_kgid(&init_user_ns, m->gid))) { kfree(settings); goto fail; }
            m->force_gid = true; continue;
        }
        if (!strncmp(token, "mode=", 5)) {
            u32 mode;
            if (kstrtou32(token + 5, 8, &mode) || mode > 0777) { kfree(settings); goto fail; }
            m->mode = mode; m->force_mode = true; continue;
        }
        if (!strncmp(token, "prefix=", 7)) {
            char *prefix = kstrdup(token + 7, GFP_KERNEL);
            if (!prefix) { r = -ENOMEM; kfree(settings); goto fail; }
            kfree(m->prefix); m->prefix = prefix; continue;
        }
        if (!strncmp(token, "volume=", 7)) {
            if (strscpy(m->volume, token + 7, sizeof(m->volume)) < 0) { kfree(settings); goto fail; }
            continue;
        }
        if (!strcmp(token, "protect")) { m->protect = true; continue; }
        if (!strcmp(token, "mufs")) { m->mufs = true; continue; }
        if (!strcmp(token, "verbose")) { m->verbose = true; continue; }
        if (!strcmp(token, "nofilenametruncate") || !strcmp(token, "grpquota") ||
            !strcmp(token, "usrquota") || !strcmp(token, "quota") || !strcmp(token, "noquota")) continue;
        if (!strncmp(token, "bs=", 3)) { value = &options.block_bytes; token += 3; }
        else if (!strncmp(token, "reserved=", 9)) { value = &options.reserved_blocks; token += 9; }
        else if (!strncmp(token, "root=", 5)) { value = &options.root_block; token += 5; }
        else { kfree(settings); goto fail; }
        if (kstrtou32(token, 0, value)) { kfree(settings); goto fail; }
    }
    kfree(settings);
    IfsOfsIoV3 io = {sizeof(io), sb_rdonly(sb) ? IFS_OFS_READ_ONLY : 0, m,
        bdev_nr_bytes(sb->s_bdev), read_io, write_io, flush_io, allocate_io, release_io, clock_io};
    r = status(ofs_probe(m->fs, &io, &options));
    if (r) goto fail;
    OfsNode root;
    r = status(ofs_node(m->fs, m->fs->root, &root, m->fs->scratch[0]));
    if (r) goto fail;
    struct inode *inode = get_inode(sb, &root);
    if (IS_ERR(inode)) { r = PTR_ERR(inode); goto fail; }
    sb->s_root = d_make_root(inode);
    if (!sb->s_root) { r = -ENOMEM; goto fail; }
    if (m->verbose) pr_info("ofs: DOS%u B=%u blocks=%u canonical engine\n", m->fs->variant, m->fs->block_bytes, m->fs->blocks);
    return 0;
fail:
    pr_err("ofs: canonical engine refused mount (%d)\n", r);
    put_super(sb); return r;
}
static struct dentry *mount_ofs(struct file_system_type *type, int flags, const char *device, void *data) {
    return mount_bdev(type, flags, device, data, fill_super);
}
static struct file_system_type ofs_type = {
    .owner = THIS_MODULE, .name = "ofs", .mount = mount_ofs,
    .kill_sb = kill_block_super, .fs_flags = FS_REQUIRES_DEV,
};
static int __init start(void) { return register_filesystem(&ofs_type); }
static void __exit stop(void) { unregister_filesystem(&ofs_type); }
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Amiga Original File System: canonical DOS0/2/4/6 engine");
MODULE_ALIAS_FS("ofs");
