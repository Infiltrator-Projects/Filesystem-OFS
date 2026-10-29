/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_FS_LOCAL_AFFS_COMPAT_H
#define INFILTRATOR_FS_LOCAL_AFFS_COMPAT_H

#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#ifndef IFS_AMIGA_MKDIR_RETURN
#error "IFS_AMIGA_MKDIR_RETURN must be defined by the filesystem adapter"
#endif

#ifndef IFS_AMIGA_BLOCK_VALID
#error "IFS_AMIGA_BLOCK_VALID must be defined by the filesystem adapter"
#endif

/*
 * Filesystem-local Linux-side AFFS-family machinery.  This file lives inside
 * one filesystem module so changing it cannot silently alter a sibling module.
 * Format-specific validation remains supplied by that filesystem's core.
 */
#define AFFS_HEAD(bh) \
    ((struct affs_head *)(bh)->b_data)
#define AFFS_TAIL(sb, bh) \
    ((struct affs_tail *)((bh)->b_data + (sb)->s_blocksize - \
                         sizeof(struct affs_tail)))
#define AFFS_ROOT_HEAD(bh) \
    ((struct affs_root_head *)(bh)->b_data)
#define AFFS_ROOT_TAIL(sb, bh) \
    ((struct affs_root_tail *)((bh)->b_data + (sb)->s_blocksize - \
                              sizeof(struct affs_root_tail)))
#define AFFS_BLOCK(sb, bh, block) \
    (AFFS_HEAD(bh)->table[AFFS_SB(sb)->s_hashsize - 1 - (block)])

#define AFFS_CACHE_SIZE PAGE_SIZE
#define AFFS_LC_SIZE (AFFS_CACHE_SIZE / sizeof(u32) / 2U)
#define AFFS_AC_SIZE (AFFS_CACHE_SIZE / sizeof(struct affs_ext_key) / 2U)
#define AFFS_AC_MASK (AFFS_AC_SIZE - 1U)

struct affs_ext_key {
    u32 ext;
    u32 key;
};

struct affs_inode_info {
    atomic_t i_opencnt;
    struct mutex i_link_lock;
    struct mutex i_ext_lock;
#define i_hash_lock i_ext_lock
    u32 i_blkcnt;
    u32 i_extcnt;
    u32 *i_lc;
    u32 i_lc_size;
    u32 i_lc_shift;
    u32 i_lc_mask;
    struct affs_ext_key *i_ac;
    u32 i_ext_last;
    struct buffer_head *i_ext_bh;
    loff_t mmu_private;
    u32 i_protect;
    u32 i_lastalloc;
    int i_pa_cnt;
    struct inode vfs_inode;
};

struct affs_bm_info {
    u32 bm_key;
    u32 bm_free;
};

struct affs_sb_info {
    u32 s_partition_size; /* unsigned disk block keys; NOT a signed capacity */
    int s_reserved;
    u32 s_data_blksize;
    u32 s_root_block;
    int s_hashsize;
    unsigned long s_flags;
    kuid_t s_uid;
    kgid_t s_gid;
    umode_t s_mode;
    struct buffer_head *s_root_bh;
    struct mutex s_bmlock;
    struct affs_bm_info *s_bitmap;
    u32 s_bmap_count;
    u32 s_bmap_bits;
    u32 s_last_bmap;
    struct buffer_head *s_bmap_bh;
    char *s_prefix;
    char s_volume[32];
    spinlock_t symlink_lock;
    struct super_block *sb;
    int work_queued;
    struct delayed_work sb_work;
    spinlock_t work_lock;
    struct rcu_head rcu;
};

#define AFFS_MOUNT_SF_INTL        0x0001UL
#define AFFS_MOUNT_SF_BM_VALID    0x0002UL
#define AFFS_MOUNT_SF_IMMUTABLE   0x0004UL
#define AFFS_MOUNT_SF_QUIET       0x0008UL
#define AFFS_MOUNT_SF_SETUID      0x0010UL
#define AFFS_MOUNT_SF_SETGID      0x0020UL
#define AFFS_MOUNT_SF_SETMODE     0x0040UL
#define AFFS_MOUNT_SF_MUFS        0x0100UL
#define AFFS_MOUNT_SF_PREFIX      0x0400UL
#define AFFS_MOUNT_SF_VERBOSE     0x0800UL
#define AFFS_MOUNT_SF_NO_TRUNCATE 0x1000UL

#define affs_clear_opt(options, option) \
    ((options) &= ~AFFS_MOUNT_##option)
#define affs_set_opt(options, option) \
    ((options) |= AFFS_MOUNT_##option)
#define affs_test_opt(options, option) \
    ((options) & AFFS_MOUNT_##option)

static inline struct affs_inode_info *AFFS_I(struct inode *inode)
{
    return container_of(inode, struct affs_inode_info, vfs_inode);
}

static inline struct affs_sb_info *AFFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline u32 ifs_amiga_chain_budget(struct super_block *sb)
{
    const struct affs_sb_info *sbi = AFFS_SB(sb);

    if (!sbi || sbi->s_reserved < 0 ||
        sbi->s_partition_size <= (u32)sbi->s_reserved)
        return 1U;

    return sbi->s_partition_size - (u32)sbi->s_reserved;
}

void affs_mark_sb_dirty(struct super_block *sb);

int affs_insert_hash(struct inode *inode, struct buffer_head *bh);
int affs_remove_hash(struct inode *dir, struct buffer_head *bh);
int affs_remove_header(struct dentry *dentry);
u32 affs_checksum_block(struct super_block *sb, struct buffer_head *bh);
void affs_fix_checksum(struct super_block *sb, struct buffer_head *bh);
void affs_secs_to_datestamp(time64_t seconds, struct affs_date *stamp);
umode_t affs_prot_to_mode(u32 protection);
void affs_mode_to_prot(struct inode *inode);
__printf(3, 4)
void affs_error(struct super_block *sb, const char *function,
                const char *format, ...);
__printf(3, 4)
void affs_warning(struct super_block *sb, const char *function,
                  const char *format, ...);
bool affs_nofilenametruncate(const struct dentry *dentry);
int affs_check_name(const unsigned char *name, int length, bool no_truncate);
int affs_copy_name(unsigned char *destination, struct dentry *dentry);

u32 affs_count_free_blocks(struct super_block *sb);
void affs_free_block(struct super_block *sb, u32 block);
u32 affs_alloc_block(struct inode *inode, u32 goal);
int affs_init_bitmap(struct super_block *sb, int *flags);
void affs_free_bitmap(struct super_block *sb);

extern const struct export_operations affs_export_ops;
int affs_hash_name(struct super_block *sb, const u8 *name, unsigned int length);
struct dentry *affs_lookup(
    struct inode *dir, struct dentry *dentry, unsigned int flags);
int affs_unlink(struct inode *dir, struct dentry *dentry);
int affs_create(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode, bool exclusive);
IFS_AMIGA_MKDIR_RETURN affs_mkdir(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode);
int affs_rmdir(struct inode *dir, struct dentry *dentry);
int affs_link(
    struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
int affs_symlink(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, const char *target);
int affs_rename2(
    struct mnt_idmap *idmap,
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry,
    unsigned int flags);

struct inode *affs_new_inode(struct inode *dir);
int affs_notify_change(
    struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attributes);
void affs_evict_inode(struct inode *inode);
struct inode *affs_iget(struct super_block *sb, unsigned long inode_number);
int affs_write_inode(struct inode *inode, struct writeback_control *writeback);
int affs_add_entry(
    struct inode *dir, struct inode *inode,
    struct dentry *dentry, s32 type);

void affs_free_prealloc(struct inode *inode);
int affs_truncate(struct inode *inode);
int affs_file_fsync(struct file *file, loff_t start, loff_t end, int datasync);
void affs_dir_truncate(struct inode *inode);

extern const struct inode_operations affs_file_inode_operations;
extern const struct inode_operations affs_dir_inode_operations;
extern const struct inode_operations affs_symlink_inode_operations;
extern const struct file_operations affs_file_operations;
extern const struct file_operations affs_dir_operations;
extern const struct address_space_operations affs_symlink_aops;
extern const struct address_space_operations affs_aops;
extern const struct dentry_operations affs_dentry_operations;
extern const struct dentry_operations affs_intl_dentry_operations;

/* Every OFS on-disk key is unsigned BE32. Passing through an int
 * previously rejected every legitimate key with bit 31 set. */
static inline bool affs_validblock(struct super_block *sb, u32 block)
{
    return IFS_AMIGA_BLOCK_VALID(sb, block);
}

static inline void affs_set_blocksize(struct super_block *sb, int size)
{
    sb_set_blocksize(sb, size);
}

static inline struct buffer_head *affs_bread(
    struct super_block *sb, u32 block)
{
    if (!affs_validblock(sb, block))
        return NULL;
    return sb_bread(sb, block);
}

static inline struct buffer_head *affs_getblk(
    struct super_block *sb, u32 block)
{
    if (!affs_validblock(sb, block))
        return NULL;
    return sb_getblk(sb, block);
}

static inline struct buffer_head *affs_getzeroblk(
    struct super_block *sb, u32 block)
{
    struct buffer_head *bh;

    if (!affs_validblock(sb, block))
        return NULL;

    bh = sb_getblk(sb, block);
    if (!bh)
        return NULL;

    lock_buffer(bh);
    memset(bh->b_data, 0, sb->s_blocksize);
    set_buffer_uptodate(bh);
    unlock_buffer(bh);
    return bh;
}

static inline struct buffer_head *affs_getemptyblk(
    struct super_block *sb, u32 block)
{
    struct buffer_head *bh;

    if (!affs_validblock(sb, block))
        return NULL;

    bh = sb_getblk(sb, block);
    if (!bh)
        return NULL;

    wait_on_buffer(bh);
    set_buffer_uptodate(bh);
    return bh;
}

static inline void affs_brelse(struct buffer_head *bh)
{
    brelse(bh);
}

static inline void affs_adjust_checksum(struct buffer_head *bh, u32 delta)
{
    __be32 *words = (__be32 *)bh->b_data;
    const u32 checksum = be32_to_cpu(words[5]);

    words[5] = cpu_to_be32(checksum - delta);
}

static inline void affs_adjust_bitmapchecksum(
    struct buffer_head *bh, u32 delta)
{
    __be32 *words = (__be32 *)bh->b_data;
    const u32 checksum = be32_to_cpu(words[0]);

    words[0] = cpu_to_be32(checksum - delta);
}

static inline void affs_lock_link(struct inode *inode)
{
    mutex_lock(&AFFS_I(inode)->i_link_lock);
}

static inline void affs_unlock_link(struct inode *inode)
{
    mutex_unlock(&AFFS_I(inode)->i_link_lock);
}

static inline void affs_lock_dir(struct inode *inode)
{
    mutex_lock_nested(
        &AFFS_I(inode)->i_hash_lock, SINGLE_DEPTH_NESTING);
}

static inline void affs_unlock_dir(struct inode *inode)
{
    mutex_unlock(&AFFS_I(inode)->i_hash_lock);
}

static inline void affs_lock_ext(struct inode *inode)
{
    mutex_lock(&AFFS_I(inode)->i_ext_lock);
}

static inline void affs_unlock_ext(struct inode *inode)
{
    mutex_unlock(&AFFS_I(inode)->i_ext_lock);
}

#endif
