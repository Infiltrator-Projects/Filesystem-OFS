/* Infiltrator Filesystems — OFS Linux namespace adapter.
 * Object metadata, directory walking, namespace mutation and symlink handling
 * are one Linux-facing namespace responsibility.
 */

/*
 * Project-authored Linux object-metadata adapter for Amiga OFS. Format-level checksum and name rules are delegated to the
 * canonical OFS core.
 */

#include "linux_adapter.h"

#include <linux/math64.h>
#include <linux/iversion.h>

static int ifs_ofs_hash_slot(
    struct super_block *sb, const unsigned char *name, unsigned int length)
{
    const int slot = affs_hash_name(sb, name, length);

    if (slot < 0 || slot >= AFFS_SB(sb)->s_hashsize)
        return -EUCLEAN;
    return slot;
}

int affs_insert_hash(struct inode *dir, struct buffer_head *entry_bh)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *cursor = NULL;
    struct affs_tail *entry_tail = AFFS_TAIL(sb, entry_bh);
    const u32 entry_block = (u32)entry_bh->b_blocknr;
    u32 next;
    u32 budget = ifs_ofs_chain_budget(sb);
    int slot;

    slot = ifs_ofs_hash_slot(
        sb, entry_tail->name + 1, entry_tail->name[0]);
    if (slot < 0)
        return slot;

    cursor = affs_bread(sb, (u32)dir->i_ino);
    if (!cursor)
        return -EIO;

    for (;;) {
        if (cursor->b_blocknr == dir->i_ino)
            next = be32_to_cpu(AFFS_HEAD(cursor)->table[slot]);
        else
            next = be32_to_cpu(AFFS_TAIL(sb, cursor)->hash_chain);

        if (next == 0U)
            break;
        if (budget-- == 0U) {
            affs_brelse(cursor);
            return -EUCLEAN;
        }

        affs_brelse(cursor);
        cursor = affs_bread(sb, next);
        if (!cursor)
            return -EIO;
    }

    entry_tail->parent = cpu_to_be32((u32)dir->i_ino);
    entry_tail->hash_chain = 0;
    affs_fix_checksum(sb, entry_bh);

    if (cursor->b_blocknr == dir->i_ino)
        AFFS_HEAD(cursor)->table[slot] = cpu_to_be32(entry_block);
    else
        AFFS_TAIL(sb, cursor)->hash_chain = cpu_to_be32(entry_block);

    affs_adjust_checksum(cursor, entry_block);
    mark_buffer_dirty_inode(cursor, dir);
    affs_brelse(cursor);

    inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
    inode_inc_iversion(dir);
    mark_inode_dirty(dir);
    return 0;
}

int affs_remove_hash(struct inode *dir, struct buffer_head *remove_bh)
{
    struct super_block *sb = dir->i_sb;
    struct affs_tail *remove_tail = AFFS_TAIL(sb, remove_bh);
    struct buffer_head *cursor = NULL;
    const u32 remove_block = (u32)remove_bh->b_blocknr;
    const __be32 replacement = remove_tail->hash_chain;
    u32 next;
    u32 budget = ifs_ofs_chain_budget(sb);
    int slot;
    int result = -ENOENT;

    slot = ifs_ofs_hash_slot(
        sb, remove_tail->name + 1, remove_tail->name[0]);
    if (slot < 0)
        return slot;

    cursor = affs_bread(sb, (u32)dir->i_ino);
    if (!cursor)
        return -EIO;

    for (;;) {
        __be32 *link_field;

        if (cursor->b_blocknr == dir->i_ino)
            link_field = &AFFS_HEAD(cursor)->table[slot];
        else
            link_field = &AFFS_TAIL(sb, cursor)->hash_chain;

        next = be32_to_cpu(*link_field);
        if (next == 0U)
            break;

        if (next == remove_block) {
            *link_field = replacement;
            affs_adjust_checksum(
                cursor, be32_to_cpu(replacement) - remove_block);
            mark_buffer_dirty_inode(cursor, dir);
            remove_tail->parent = 0;
            affs_fix_checksum(sb, remove_bh);
            result = 0;
            break;
        }

        if (budget-- == 0U) {
            result = -EUCLEAN;
            break;
        }

        affs_brelse(cursor);
        cursor = affs_bread(sb, next);
        if (!cursor)
            return -EIO;
    }

    affs_brelse(cursor);

    if (result == 0) {
        inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
        inode_inc_iversion(dir);
        mark_inode_dirty(dir);
    }
    return result;
}

static void ifs_ofs_retarget_cached_dentry(
    struct inode *inode, const u32 old_block)
{
    struct dentry *alias;

    spin_lock(&inode->i_lock);
    hlist_for_each_entry(alias, &inode->i_dentry, d_u.d_alias) {
        if (old_block == (u32)(unsigned long)alias->d_fsdata) {
            alias->d_fsdata = (void *)(unsigned long)inode->i_ino;
            break;
        }
    }
    spin_unlock(&inode->i_lock);
}

static int ifs_ofs_directory_empty(struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    int index;
    int result = 0;

    bh = affs_bread(sb, (u32)inode->i_ino);
    if (!bh)
        return -EIO;

    for (index = 0; index < AFFS_SB(sb)->s_hashsize; ++index) {
        if (AFFS_HEAD(bh)->table[index] != 0) {
            result = -ENOTEMPTY;
            break;
        }
    }

    affs_brelse(bh);
    return result;
}

static int ifs_ofs_promote_link_head(
    struct inode *inode,
    struct buffer_head *head_bh,
    const u32 promoted_block,
    struct buffer_head **promoted_bh_out)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *promoted_bh;
    struct inode *dir;
    unsigned char original_name[32];
    int result;

    promoted_bh = affs_bread(sb, promoted_block);
    if (!promoted_bh)
        return -EIO;

    dir = affs_iget(
        sb, be32_to_cpu(AFFS_TAIL(sb, promoted_bh)->parent));
    if (IS_ERR(dir)) {
        result = PTR_ERR(dir);
        affs_brelse(promoted_bh);
        return result;
    }

    memcpy(original_name, AFFS_TAIL(sb, head_bh)->name,
           sizeof(original_name));

    affs_lock_dir(dir);
    ifs_ofs_retarget_cached_dentry(inode, promoted_block);

    result = affs_remove_hash(dir, promoted_bh);
    if (result != 0)
        goto out_unlock;

    memcpy(AFFS_TAIL(sb, head_bh)->name,
           AFFS_TAIL(sb, promoted_bh)->name, 32U);
    affs_fix_checksum(sb, head_bh);

    result = affs_insert_hash(dir, head_bh);
    if (result != 0) {
        memcpy(AFFS_TAIL(sb, head_bh)->name,
               original_name, sizeof(original_name));
        affs_fix_checksum(sb, head_bh);
        if (affs_insert_hash(dir, promoted_bh) != 0)
            affs_error(sb, "ifs_ofs_promote_link_head",
                       "Could not restore link after promotion failure");
        goto out_unlock;
    }

    mark_buffer_dirty_inode(head_bh, inode);
    mark_buffer_dirty_inode(promoted_bh, inode);

out_unlock:
    affs_unlock_dir(dir);
    iput(dir);

    if (result != 0) {
        affs_brelse(promoted_bh);
        return result;
    }

    *promoted_bh_out = promoted_bh;
    return 0;
}

static int ifs_ofs_remove_link(struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *cursor = NULL;
    struct buffer_head *link_bh = NULL;
    u32 link_block = (u32)(unsigned long)dentry->d_fsdata;
    u32 next;
    u32 budget = ifs_ofs_chain_budget(sb);
    int result = -ENOENT;

    cursor = affs_bread(sb, (u32)inode->i_ino);
    if (!cursor)
        return -EIO;

    if (link_block == (u32)inode->i_ino) {
        link_block = be32_to_cpu(AFFS_TAIL(sb, cursor)->link_chain);
        if (link_block == 0U) {
            result = -EUCLEAN;
            goto out;
        }

        result = ifs_ofs_promote_link_head(
            inode, cursor, link_block, &link_bh);
        if (result != 0)
            goto out;
    } else {
        link_bh = affs_bread(sb, link_block);
        if (!link_bh) {
            result = -EIO;
            goto out;
        }
    }

    for (;;) {
        next = be32_to_cpu(AFFS_TAIL(sb, cursor)->link_chain);
        if (next == 0U)
            break;

        if (next == link_block) {
            const __be32 replacement =
                AFFS_TAIL(sb, link_bh)->link_chain;

            AFFS_TAIL(sb, cursor)->link_chain = replacement;
            affs_adjust_checksum(
                cursor, be32_to_cpu(replacement) - link_block);
            mark_buffer_dirty_inode(cursor, inode);

            switch (be32_to_cpu(AFFS_TAIL(sb, cursor)->stype)) {
            case ST_LINKDIR:
            case ST_LINKFILE:
                break;
            default:
                if (replacement == 0)
                    set_nlink(inode, 1);
                break;
            }

            affs_free_block(sb, link_block);
            result = 0;
            break;
        }

        if (budget-- == 0U) {
            result = -EUCLEAN;
            break;
        }

        affs_brelse(cursor);
        cursor = affs_bread(sb, next);
        if (!cursor) {
            result = -EIO;
            break;
        }
    }

out:
    affs_brelse(link_bh);
    affs_brelse(cursor);
    return result;
}

int affs_remove_header(struct dentry *dentry)
{
    struct inode *dir = d_inode(dentry->d_parent);
    struct inode *inode = d_inode(dentry);
    struct super_block *sb;
    struct buffer_head *bh = NULL;
    int result;

    if (!dir || !inode)
        return -ENOENT;

    sb = dir->i_sb;
    bh = affs_bread(sb, (u32)(unsigned long)dentry->d_fsdata);
    if (!bh)
        return -EIO;

    affs_lock_link(inode);
    affs_lock_dir(dir);

    if (be32_to_cpu(AFFS_TAIL(sb, bh)->stype) == ST_USERDIR) {
        affs_lock_dir(inode);
        result = ifs_ofs_directory_empty(inode);
        affs_unlock_dir(inode);
        if (result != 0)
            goto out_locked;
    }

    result = affs_remove_hash(dir, bh);
    if (result != 0)
        goto out_locked;

    mark_buffer_dirty_inode(bh, inode);
    affs_unlock_dir(dir);

    if (inode->i_nlink > 1)
        result = ifs_ofs_remove_link(dentry);
    else {
        clear_nlink(inode);
        result = 0;
    }

    affs_unlock_link(inode);
    inode_set_ctime_current(inode);
    mark_inode_dirty(inode);
    affs_brelse(bh);
    return result;

out_locked:
    affs_unlock_dir(dir);
    affs_unlock_link(inode);
    affs_brelse(bh);
    return result;
}

u32 affs_checksum_block(struct super_block *sb, struct buffer_head *bh)
{
    return ifs_ofs_block_checksum(
        (const ifs_ofs_u8 *)bh->b_data, (ifs_ofs_u32)sb->s_blocksize);
}

void affs_fix_checksum(struct super_block *sb, struct buffer_head *bh)
{
    __be32 *words = (__be32 *)bh->b_data;

    words[5] = cpu_to_be32(
        ifs_ofs_checksum_word_value(
            (const ifs_ofs_u8 *)bh->b_data,
            (ifs_ofs_u32)sb->s_blocksize, 5U));
}

void affs_secs_to_datestamp(time64_t seconds, struct affs_date *stamp)
{
    u32 days;
    u32 minutes;
    s32 remainder;

    seconds -= (time64_t)sys_tz.tz_minuteswest * 60 + AFFS_EPOCH_DELTA;
    if (seconds < 0)
        seconds = 0;

    days = (u32)div_s64_rem(seconds, 86400, &remainder);
    minutes = (u32)(remainder / 60);
    remainder -= (s32)minutes * 60;

    stamp->days = cpu_to_be32(days);
    stamp->mins = cpu_to_be32(minutes);
    stamp->ticks = cpu_to_be32((u32)remainder * 50U);
}

umode_t affs_prot_to_mode(u32 protection)
{
    umode_t mode = 0;

    if ((protection & FIBF_NOREAD) == 0U)
        mode |= 0400;
    if ((protection & FIBF_NOWRITE) == 0U)
        mode |= 0200;
    if ((protection & FIBF_NOEXECUTE) == 0U)
        mode |= 0100;

    if ((protection & FIBF_GRP_READ) != 0U)
        mode |= 0040;
    if ((protection & FIBF_GRP_WRITE) != 0U)
        mode |= 0020;
    if ((protection & FIBF_GRP_EXECUTE) != 0U)
        mode |= 0010;

    if ((protection & FIBF_OTR_READ) != 0U)
        mode |= 0004;
    if ((protection & FIBF_OTR_WRITE) != 0U)
        mode |= 0002;
    if ((protection & FIBF_OTR_EXECUTE) != 0U)
        mode |= 0001;

    return mode;
}

void affs_mode_to_prot(struct inode *inode)
{
    const umode_t mode = inode->i_mode;
    u32 protection = AFFS_I(inode)->i_protect;

    protection &= ~(FIBF_NOEXECUTE | FIBF_NOREAD | FIBF_NOWRITE |
                    FIBF_NODELETE | FIBF_GRP_EXECUTE | FIBF_GRP_READ |
                    FIBF_GRP_WRITE | FIBF_GRP_DELETE |
                    FIBF_OTR_EXECUTE | FIBF_OTR_READ |
                    FIBF_OTR_WRITE | FIBF_OTR_DELETE);

    if ((mode & 0100) == 0)
        protection |= FIBF_NOEXECUTE;
    if ((mode & 0400) == 0)
        protection |= FIBF_NOREAD;
    if ((mode & 0200) == 0)
        protection |= FIBF_NOWRITE;

    if ((mode & 0010) != 0)
        protection |= FIBF_GRP_EXECUTE;
    if ((mode & 0040) != 0)
        protection |= FIBF_GRP_READ;
    if ((mode & 0020) != 0)
        protection |= FIBF_GRP_WRITE;
    if ((mode & 0070) != 0)
        protection |= FIBF_GRP_DELETE;

    if ((mode & 0001) != 0)
        protection |= FIBF_OTR_EXECUTE;
    if ((mode & 0004) != 0)
        protection |= FIBF_OTR_READ;
    if ((mode & 0002) != 0)
        protection |= FIBF_OTR_WRITE;
    if ((mode & 0007) != 0)
        protection |= FIBF_OTR_DELETE;

    AFFS_I(inode)->i_protect = protection;
}

void affs_error(struct super_block *sb, const char *function,
                const char *format, ...)
{
    struct va_format message;
    va_list arguments;

    va_start(arguments, format);
    message.fmt = format;
    message.va = &arguments;
    pr_crit("error (device %s): %s(): %pV\n",
            sb->s_id, function, &message);
    if (!sb_rdonly(sb))
        pr_warn("Remounting filesystem read-only\n");
    sb->s_flags |= SB_RDONLY;
    va_end(arguments);
}

void affs_warning(struct super_block *sb, const char *function,
                  const char *format, ...)
{
    struct va_format message;
    va_list arguments;

    va_start(arguments, format);
    message.fmt = format;
    message.va = &arguments;
    pr_warn("(device %s): %s(): %pV\n",
            sb->s_id, function, &message);
    va_end(arguments);
}

bool affs_nofilenametruncate(const struct dentry *dentry)
{
    return affs_test_opt(
        AFFS_SB(dentry->d_sb)->s_flags, SF_NO_TRUNCATE) != 0;
}

int affs_check_name(const unsigned char *name, int length, bool no_truncate)
{
    const IfsOfsNameStatus status = ifs_ofs_validate_name(
        name, length < 0 ? 0U : (ifs_ofs_u32)length,
        no_truncate ? 1 : 0);

    if (length < 0)
        return -EINVAL;

    switch (status) {
    case IFS_OFS_NAME_OK:
        return 0;
    case IFS_OFS_NAME_TOO_LONG:
        return -ENAMETOOLONG;
    case IFS_OFS_NAME_INVALID_CHARACTER:
    default:
        return -EINVAL;
    }
}

int affs_copy_name(unsigned char *destination, struct dentry *dentry)
{
    const u32 length =
        min_t(u32, (u32)dentry->d_name.len, (u32)AFFSNAMEMAX);

    destination[0] = (unsigned char)length;
    memcpy(destination + 1, dentry->d_name.name, length);
    return (int)length;
}


/* ===== directory enumeration ===== */
/*
 * Project-authored Linux directory adapter for AmigaDOS OFS/FFS media.
 *
 * The on-disk hash table and hash-chain layout are filesystem semantics.
 * This unit only binds that layout to Linux VFS directory iteration and keeps
 * the VFS cursor stable across partial reads.
 */

#include <linux/iversion.h>
#include "linux_adapter.h"

struct ifs_amiga_dir_state {
    u32 resume_block;
    u64 inode_version;
};

static int ifs_amiga_iterate_directory(struct file *file,
                                       struct dir_context *ctx);

static loff_t ifs_amiga_dir_llseek(struct file *file, loff_t offset, int whence)
{
    struct ifs_amiga_dir_state *state = file->private_data;

    return generic_llseek_cookie(file, offset, whence, &state->inode_version);
}

static int ifs_amiga_dir_open(struct inode *inode, struct file *file)
{
    struct ifs_amiga_dir_state *state;

    state = kzalloc(sizeof(*state), GFP_KERNEL);
    if (!state)
        return -ENOMEM;

    file->private_data = state;
    return 0;
}

static int ifs_amiga_dir_release(struct inode *inode, struct file *file)
{
    kfree(file->private_data);
    file->private_data = NULL;
    return 0;
}

const struct file_operations affs_dir_operations = {
    .open = ifs_amiga_dir_open,
    .read = generic_read_dir,
    .llseek = ifs_amiga_dir_llseek,
    .iterate_shared = ifs_amiga_iterate_directory,
    .fsync = affs_file_fsync,
    .release = ifs_amiga_dir_release,
};

const struct inode_operations affs_dir_inode_operations = {
    .create = affs_create,
    .lookup = affs_lookup,
    .link = affs_link,
    .unlink = affs_unlink,
    .symlink = affs_symlink,
    .mkdir = affs_mkdir,
    .rmdir = affs_rmdir,
    .rename = affs_rename2,
    .setattr = affs_notify_change,
};

static int ifs_amiga_iterate_directory(struct file *file,
                                       struct dir_context *ctx)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct ifs_amiga_dir_state *state = file->private_data;
    struct buffer_head *directory = NULL;
    struct buffer_head *entry = NULL;
    u64 cursor;
    u64 bucket64;
    u32 block = 0U;
    u32 chain_index;
    u32 bucket;
    u32 walked;
    int result = 0;

    if (ctx->pos < 2) {
        state->resume_block = 0U;
        if (!dir_emit_dots(file, ctx))
            return 0;
    }

    cursor = (u64)(ctx->pos - 2);
    bucket64 = cursor >> 16;
    chain_index = (u32)(cursor & 0xffffU);

    if (chain_index == 0xffffU) {
        bucket64++;
        chain_index = 0U;
        ctx->pos = (loff_t)(bucket64 << 16) + 2;
    }

    if (bucket64 >= (u64)sbi->s_hashsize)
        return 0;

    bucket = (u32)bucket64;

    affs_lock_dir(inode);

    directory = affs_bread(sb, inode->i_ino);
    if (!directory) {
        result = -EIO;
        goto out;
    }

    if (state->resume_block != 0U &&
        inode_eq_iversion(inode, state->inode_version)) {
        block = state->resume_block;
        goto emit_chain;
    }

    block = be32_to_cpu(AFFS_HEAD(directory)->table[bucket]);
    for (walked = 0U; block != 0U && walked < chain_index; ++walked) {
        entry = affs_bread(sb, block);
        if (!entry) {
            result = -EIO;
            goto out;
        }

        block = be32_to_cpu(AFFS_TAIL(sb, entry)->hash_chain);
        affs_brelse(entry);
        entry = NULL;
    }

    if (block != 0U)
        goto emit_chain;

    bucket++;

    for (; bucket < (u32)sbi->s_hashsize; ++bucket) {
        block = be32_to_cpu(AFFS_HEAD(directory)->table[bucket]);
        if (block == 0U)
            continue;

        ctx->pos = ((loff_t)bucket << 16) + 2;

emit_chain:
        while (block != 0U) {
            const struct affs_tail *tail;
            const unsigned char *name;
            unsigned int name_length;

            entry = affs_bread(sb, block);
            if (!entry) {
                result = -EIO;
                goto out;
            }

            tail = AFFS_TAIL(sb, entry);
            name_length = min_t(unsigned int, tail->name[0], AFFSNAMEMAX);
            name = tail->name + 1;

            if (!dir_emit(ctx, name, name_length, block, DT_UNKNOWN))
                goto save_position;

            ctx->pos++;
            block = be32_to_cpu(tail->hash_chain);
            affs_brelse(entry);
            entry = NULL;
        }
    }

save_position:
    state->inode_version = inode_query_iversion(inode);
    state->resume_block = block;

out:
    affs_brelse(entry);
    affs_brelse(directory);
    affs_unlock_dir(inode);
    return result;
}


/* ===== namespace mutation ===== */
/*
 * Project-authored Linux namespace adapter for Amiga OFS.
 * Canonical name folding, validation, directory hashing and symlink encoding
 * live in the OFS core.
 */

#include "linux_adapter.h"

#include <linux/exportfs.h>

static int ifs_ofs_name_hash(
    const struct dentry *parent, struct qstr *name, const bool international)
{
    const unsigned char *cursor = name->name;
    unsigned long hash;
    u32 remaining;
    int result;

    result = affs_check_name(
        name->name, (int)name->len, affs_nofilenametruncate(parent));
    if (result != 0)
        return result;

    hash = init_name_hash(parent);
    remaining = min_t(u32, (u32)name->len, (u32)AFFSNAMEMAX);
    while (remaining-- != 0U)
        hash = partial_name_hash(
            ifs_ofs_fold_character(*cursor++, international ? 1 : 0),
            hash);

    name->hash = end_name_hash(hash);
    return 0;
}

static int ifs_ofs_hash_dentry(
    const struct dentry *parent, struct qstr *name)
{
    return ifs_ofs_name_hash(parent, name, false);
}

static int ifs_ofs_intl_hash_dentry(
    const struct dentry *parent, struct qstr *name)
{
    return ifs_ofs_name_hash(parent, name, true);
}

static int ifs_ofs_name_compare(
    const struct dentry *parent,
    unsigned int existing_length,
    const char *existing_name,
    const struct qstr *candidate,
    const bool international)
{
    u32 length;
    u32 index;

    if (affs_check_name(
            candidate->name, (int)candidate->len,
            affs_nofilenametruncate(parent)) != 0)
        return 1;

    if (existing_length >= AFFSNAMEMAX) {
        if (candidate->len < AFFSNAMEMAX)
            return 1;
        length = AFFSNAMEMAX;
    } else {
        if (existing_length != candidate->len)
            return 1;
        length = existing_length;
    }

    for (index = 0U; index < length; ++index) {
        if (ifs_ofs_fold_character(
                (ifs_ofs_u8)existing_name[index],
                international ? 1 : 0) !=
            ifs_ofs_fold_character(
                candidate->name[index], international ? 1 : 0))
            return 1;
    }

    return 0;
}

static int ifs_ofs_compare_dentry(
    const struct dentry *parent, unsigned int length,
    const char *existing, const struct qstr *candidate)
{
    return ifs_ofs_name_compare(
        parent, length, existing, candidate, false);
}

static int ifs_ofs_intl_compare_dentry(
    const struct dentry *parent, unsigned int length,
    const char *existing, const struct qstr *candidate)
{
    return ifs_ofs_name_compare(
        parent, length, existing, candidate, true);
}

static bool ifs_ofs_disk_name_matches(
    const struct dentry *dentry,
    const u8 *disk_name,
    const bool international)
{
    u32 length = (u32)dentry->d_name.len;
    u32 disk_length = disk_name[0];
    u32 index;

    if (length >= AFFSNAMEMAX) {
        if (disk_length < AFFSNAMEMAX)
            return false;
        length = AFFSNAMEMAX;
    } else if (length != disk_length) {
        return false;
    }

    for (index = 0U; index < length; ++index) {
        if (ifs_ofs_fold_character(
                dentry->d_name.name[index], international ? 1 : 0) !=
            ifs_ofs_fold_character(
                disk_name[index + 1U], international ? 1 : 0))
            return false;
    }

    return true;
}

int affs_hash_name(struct super_block *sb, const u8 *name, unsigned int length)
{
    return (int)ifs_ofs_directory_hash(
        name, (ifs_ofs_u32)length,
        (ifs_ofs_u32)AFFS_SB(sb)->s_hashsize,
        affs_test_opt(AFFS_SB(sb)->s_flags, SF_INTL) ? 1 : 0);
}

static struct buffer_head *ifs_ofs_find_entry(
    struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *bh;
    const bool international =
        affs_test_opt(AFFS_SB(sb)->s_flags, SF_INTL) != 0;
    const int slot = affs_hash_name(
        sb, dentry->d_name.name, dentry->d_name.len);
    u32 key;
    u32 budget;

    if (slot < 0 || slot >= AFFS_SB(sb)->s_hashsize)
        return ERR_PTR(-EUCLEAN);

    bh = affs_bread(sb, (u32)dir->i_ino);
    if (!bh)
        return ERR_PTR(-EIO);

    key = be32_to_cpu(AFFS_HEAD(bh)->table[slot]);
    affs_brelse(bh);
    budget = ifs_ofs_chain_budget(sb);

    while (key != 0U) {
        if (budget-- == 0U)
            return ERR_PTR(-EUCLEAN);

        bh = affs_bread(sb, key);
        if (!bh)
            return ERR_PTR(-EIO);

        if (ifs_ofs_disk_name_matches(
                dentry, AFFS_TAIL(sb, bh)->name, international))
            return bh;

        key = be32_to_cpu(AFFS_TAIL(sb, bh)->hash_chain);
        affs_brelse(bh);
    }

    return NULL;
}

struct dentry *affs_lookup(
    struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *bh;
    struct inode *inode = NULL;
    struct dentry *result;

    affs_lock_dir(dir);
    bh = ifs_ofs_find_entry(dir, dentry);
    if (IS_ERR(bh)) {
        affs_unlock_dir(dir);
        return ERR_CAST(bh);
    }

    if (bh) {
        u32 inode_block = (u32)bh->b_blocknr;
        const u32 type = be32_to_cpu(AFFS_TAIL(sb, bh)->stype);

        dentry->d_fsdata = (void *)(unsigned long)inode_block;
        if (type == ST_LINKFILE)
            inode_block = be32_to_cpu(AFFS_TAIL(sb, bh)->original);

        affs_brelse(bh);
        inode = affs_iget(sb, inode_block);
    }

    result = d_splice_alias(inode, dentry);
    if (!IS_ERR_OR_NULL(result))
        result->d_fsdata = dentry->d_fsdata;

    affs_unlock_dir(dir);
    return result;
}

int affs_unlink(struct inode *dir, struct dentry *dentry)
{
    return affs_remove_header(dentry);
}

int affs_create(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode, bool exclusive)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode;
    int result;

    inode = affs_new_inode(dir);
    if (!inode)
        return -ENOSPC;

    inode->i_mode = mode;
    affs_mode_to_prot(inode);
    mark_inode_dirty(inode);

    inode->i_op = &affs_file_inode_operations;
    inode->i_fop = &affs_file_operations;
    inode->i_mapping->a_ops =
        affs_test_opt(AFFS_SB(sb)->s_flags, SF_OFS) ?
        &affs_aops_ofs : &affs_aops;

    result = affs_add_entry(dir, inode, dentry, ST_FILE);
    if (result != 0) {
        clear_nlink(inode);
        iput(inode);
    }
    return result;
}

IFS_OFS_MKDIR_RETURN affs_mkdir(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
{
    struct inode *inode;
    int result;

    inode = affs_new_inode(dir);
    if (!inode)
        return IFS_OFS_MKDIR_FAILURE(-ENOSPC);

    inode->i_mode = S_IFDIR | mode;
    affs_mode_to_prot(inode);
    inode->i_op = &affs_dir_inode_operations;
    inode->i_fop = &affs_dir_operations;

    result = affs_add_entry(dir, inode, dentry, ST_USERDIR);
    if (result != 0) {
        clear_nlink(inode);
        mark_inode_dirty(inode);
        iput(inode);
        return IFS_OFS_MKDIR_FAILURE(result);
    }
    return IFS_OFS_MKDIR_SUCCESS;
}

int affs_rmdir(struct inode *dir, struct dentry *dentry)
{
    return affs_remove_header(dentry);
}

int affs_symlink(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, const char *target)
{
    struct super_block *sb = dir->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh = NULL;
    struct inode *inode;
    ifs_ofs_u32 encoded_length = 0U;
    IfsOfsSymlinkStatus status;
    size_t target_length;
    size_t volume_length;
    u32 capacity;
    int result;

    target_length = strlen(target);
    if (target_length >= U32_MAX)
        return -ENAMETOOLONG;

    inode = affs_new_inode(dir);
    if (!inode)
        return -ENOSPC;

    inode->i_op = &affs_symlink_inode_operations;
    inode_nohighmem(inode);
    inode->i_data.a_ops = &affs_symlink_aops;
    inode->i_mode = S_IFLNK | 0777;
    affs_mode_to_prot(inode);

    bh = affs_bread(sb, (u32)inode->i_ino);
    if (!bh) {
        result = -EIO;
        goto fail;
    }

    capacity = (u32)sbi->s_hashsize * sizeof(u32);
    spin_lock(&sbi->symlink_lock);
    volume_length = strnlen(sbi->s_volume, sizeof(sbi->s_volume));
    status = ifs_ofs_encode_symlink(
        (const ifs_ofs_u8 *)target,
        (ifs_ofs_u32)target_length + 1U,
        (const ifs_ofs_u8 *)sbi->s_volume,
        (ifs_ofs_u32)volume_length,
        (ifs_ofs_u8 *)AFFS_HEAD(bh)->table,
        capacity, &encoded_length);
    spin_unlock(&sbi->symlink_lock);

    if (status != IFS_OFS_SYMLINK_OK) {
        result = status == IFS_OFS_SYMLINK_OUTPUT_TOO_SMALL ?
                 -ENAMETOOLONG : -EINVAL;
        goto fail;
    }

    inode->i_size = encoded_length + 1U;
    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    bh = NULL;
    mark_inode_dirty(inode);

    result = affs_add_entry(dir, inode, dentry, ST_SOFTLINK);
    if (result == 0)
        return 0;

fail:
    affs_brelse(bh);
    clear_nlink(inode);
    mark_inode_dirty(inode);
    iput(inode);
    return result;
}

int affs_link(
    struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
    return affs_add_entry(dir, d_inode(old_dentry), dentry, ST_LINKFILE);
}

static u32 ifs_ofs_dentry_block(const struct dentry *dentry)
{
    const unsigned long stored = (unsigned long)dentry->d_fsdata;

    return stored != 0UL ? (u32)stored : (u32)d_inode(dentry)->i_ino;
}

static int ifs_ofs_remove_from_directory(
    struct inode *dir, struct buffer_head *bh)
{
    int result;

    affs_lock_dir(dir);
    result = affs_remove_hash(dir, bh);
    affs_unlock_dir(dir);
    return result;
}

static int ifs_ofs_insert_into_directory(
    struct inode *dir, struct buffer_head *bh)
{
    int result;

    affs_lock_dir(dir);
    result = affs_insert_hash(dir, bh);
    affs_unlock_dir(dir);
    return result;
}

static int ifs_ofs_rename(
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry)
{
    struct super_block *sb = old_dir->i_sb;
    struct buffer_head *bh;
    unsigned char old_name[32];
    int result;
    int rollback;

    result = affs_check_name(
        new_dentry->d_name.name, (int)new_dentry->d_name.len,
        affs_nofilenametruncate(old_dentry));
    if (result != 0)
        return result;

    if (d_really_is_positive(new_dentry)) {
        result = affs_remove_header(new_dentry);
        if (result != 0)
            return result;
    }

    bh = affs_bread(sb, ifs_ofs_dentry_block(old_dentry));
    if (!bh)
        return -EIO;

    memcpy(old_name, AFFS_TAIL(sb, bh)->name, sizeof(old_name));

    result = ifs_ofs_remove_from_directory(old_dir, bh);
    if (result != 0)
        goto out;

    affs_copy_name(AFFS_TAIL(sb, bh)->name, new_dentry);
    affs_fix_checksum(sb, bh);
    result = ifs_ofs_insert_into_directory(new_dir, bh);
    if (result == 0)
        goto out;

    memcpy(AFFS_TAIL(sb, bh)->name, old_name, sizeof(old_name));
    affs_fix_checksum(sb, bh);
    rollback = ifs_ofs_insert_into_directory(old_dir, bh);
    if (rollback != 0)
        affs_error(sb, "ifs_ofs_rename",
                   "Could not restore source after failed rename");

out:
    mark_buffer_dirty_inode(bh, result == 0 ? new_dir : old_dir);
    affs_brelse(bh);
    return result;
}

static int ifs_ofs_exchange(
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry)
{
    struct super_block *sb = old_dir->i_sb;
    struct buffer_head *old_bh = NULL;
    struct buffer_head *new_bh = NULL;
    unsigned char old_name[32];
    unsigned char new_name[32];
    bool old_removed = false;
    bool new_removed = false;
    bool old_inserted_new = false;
    int result;

    if (!d_really_is_positive(new_dentry))
        return -ENOENT;

    old_bh = affs_bread(sb, ifs_ofs_dentry_block(old_dentry));
    new_bh = affs_bread(sb, ifs_ofs_dentry_block(new_dentry));
    if (!old_bh || !new_bh) {
        result = -EIO;
        goto out;
    }

    memcpy(old_name, AFFS_TAIL(sb, old_bh)->name, sizeof(old_name));
    memcpy(new_name, AFFS_TAIL(sb, new_bh)->name, sizeof(new_name));

    result = ifs_ofs_remove_from_directory(old_dir, old_bh);
    if (result != 0)
        goto out;
    old_removed = true;

    result = ifs_ofs_remove_from_directory(new_dir, new_bh);
    if (result != 0)
        goto rollback;
    new_removed = true;

    affs_copy_name(AFFS_TAIL(sb, old_bh)->name, new_dentry);
    affs_fix_checksum(sb, old_bh);
    result = ifs_ofs_insert_into_directory(new_dir, old_bh);
    if (result != 0)
        goto rollback;
    old_inserted_new = true;

    affs_copy_name(AFFS_TAIL(sb, new_bh)->name, old_dentry);
    affs_fix_checksum(sb, new_bh);
    result = ifs_ofs_insert_into_directory(old_dir, new_bh);
    if (result == 0)
        goto out;

rollback:
    if (old_inserted_new)
        (void)ifs_ofs_remove_from_directory(new_dir, old_bh);

    memcpy(AFFS_TAIL(sb, old_bh)->name, old_name, sizeof(old_name));
    memcpy(AFFS_TAIL(sb, new_bh)->name, new_name, sizeof(new_name));
    affs_fix_checksum(sb, old_bh);
    affs_fix_checksum(sb, new_bh);

    if (old_removed &&
        ifs_ofs_insert_into_directory(old_dir, old_bh) != 0)
        affs_error(sb, "ifs_ofs_exchange",
                   "Could not restore first entry after failed exchange");

    if (new_removed &&
        ifs_ofs_insert_into_directory(new_dir, new_bh) != 0)
        affs_error(sb, "ifs_ofs_exchange",
                   "Could not restore second entry after failed exchange");

out:
    if (old_bh)
        mark_buffer_dirty_inode(old_bh, old_dir);
    if (new_bh)
        mark_buffer_dirty_inode(new_bh, new_dir);
    affs_brelse(old_bh);
    affs_brelse(new_bh);
    return result;
}

int affs_rename2(
    struct mnt_idmap *idmap,
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry,
    unsigned int flags)
{
    if ((flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) != 0U)
        return -EINVAL;

    if ((flags & RENAME_NOREPLACE) != 0U &&
        d_really_is_positive(new_dentry))
        return -EEXIST;

    if ((flags & RENAME_EXCHANGE) != 0U)
        return ifs_ofs_exchange(
            old_dir, old_dentry, new_dir, new_dentry);

    return ifs_ofs_rename(
        old_dir, old_dentry, new_dir, new_dentry);
}

static struct dentry *ifs_ofs_get_parent(struct dentry *child)
{
    struct buffer_head *bh;
    struct inode *parent;
    const u32 block = (u32)d_inode(child)->i_ino;

    bh = affs_bread(child->d_sb, block);
    if (!bh)
        return ERR_PTR(-EIO);

    parent = affs_iget(
        child->d_sb, be32_to_cpu(AFFS_TAIL(child->d_sb, bh)->parent));
    affs_brelse(bh);
    return d_obtain_alias(parent);
}

static struct inode *ifs_ofs_export_inode(
    struct super_block *sb, u64 inode_number, u32 generation)
{
    if (inode_number > U32_MAX ||
        !affs_validblock(sb, (u32)inode_number))
        return ERR_PTR(-ESTALE);

    return affs_iget(sb, (unsigned long)inode_number);
}

static struct dentry *ifs_ofs_fh_to_dentry(
    struct super_block *sb, struct fid *fid, int length, int type)
{
    return generic_fh_to_dentry(
        sb, fid, length, type, ifs_ofs_export_inode);
}

static struct dentry *ifs_ofs_fh_to_parent(
    struct super_block *sb, struct fid *fid, int length, int type)
{
    return generic_fh_to_parent(
        sb, fid, length, type, ifs_ofs_export_inode);
}

const struct export_operations affs_export_ops = {
    .encode_fh = generic_encode_ino32_fh,
    .fh_to_dentry = ifs_ofs_fh_to_dentry,
    .fh_to_parent = ifs_ofs_fh_to_parent,
    .get_parent = ifs_ofs_get_parent,
};

const struct dentry_operations affs_dentry_operations = {
    .d_hash = ifs_ofs_hash_dentry,
    .d_compare = ifs_ofs_compare_dentry,
};

const struct dentry_operations affs_intl_dentry_operations = {
    .d_hash = ifs_ofs_intl_hash_dentry,
    .d_compare = ifs_ofs_intl_compare_dentry,
};


/* ===== symlink integration ===== */
/*
 * Project-authored Linux adapter for canonical AmigaDOS symlink semantics.
 * Filesystem path interpretation lives in the OFS core.
 */

#include "linux_adapter.h"

#define IFS_OFS_SYMLINK_MAX 1024U

static int affs_symlink_read_folio(struct file *file, struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    struct affs_sb_info *sbi = AFFS_SB(inode->i_sb);
    struct buffer_head *bh;
    struct slink_front *front;
    const char *prefix;
    char *link = folio_address(folio);
    size_t source_capacity;
    size_t prefix_length;
    size_t output_capacity;
    ifs_ofs_u32 output_length = 0U;
    IfsOfsSymlinkStatus status;

    pr_debug("get_link(ino=%lu)\n", inode->i_ino);

    bh = affs_bread(inode->i_sb, inode->i_ino);
    if (!bh)
        goto io_error;

    if (bh->b_size <= sizeof(*front))
        goto malformed;

    front = (struct slink_front *)bh->b_data;
    source_capacity = bh->b_size - sizeof(*front);
    output_capacity = min_t(size_t, folio_size(folio),
                            (size_t)IFS_OFS_SYMLINK_MAX);

    spin_lock(&sbi->symlink_lock);
    prefix = sbi->s_prefix ? sbi->s_prefix : "/";
    prefix_length = strnlen(prefix, IFS_OFS_SYMLINK_MAX);

    status = ifs_ofs_translate_symlink(
        front->symname,
        (ifs_ofs_u32)source_capacity,
        (const ifs_ofs_u8 *)prefix,
        (ifs_ofs_u32)prefix_length,
        (ifs_ofs_u8 *)link,
        (ifs_ofs_u32)output_capacity,
        &output_length);
    spin_unlock(&sbi->symlink_lock);

    affs_brelse(bh);

    if (status != IFS_OFS_SYMLINK_OK)
        goto io_error_no_buffer;

    folio_mark_uptodate(folio);
    folio_unlock(folio);
    return 0;

malformed:
    affs_brelse(bh);
io_error_no_buffer:
    folio_unlock(folio);
    return -EIO;

io_error:
    folio_unlock(folio);
    return -EIO;
}

const struct address_space_operations affs_symlink_aops = {
    .read_folio = affs_symlink_read_folio,
};

const struct inode_operations affs_symlink_inode_operations = {
    .get_link = page_get_link,
    .setattr = affs_notify_change,
};
