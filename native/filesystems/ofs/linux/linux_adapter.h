#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "../core/ofs_disk_layout.h"
#ifndef INFILTRATOR_OFS_LINUX_ADAPTER_H
#define INFILTRATOR_OFS_LINUX_ADAPTER_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include "../core/ofs_primitives.h"
#include "../../../platform/linux_kernel/vfs_compat.h"

/* Kernel API compatibility is shared; AFFS-family state is filesystem-local. */
#define IFS_OFS_AOPS_WRITE_CONTEXT IFS_LINUX_AOPS_WRITE_CONTEXT
#define IFS_OFS_AOPS_WRITE_CONTEXT_ARG IFS_LINUX_AOPS_WRITE_CONTEXT_ARG
#define IFS_OFS_INODE_IS_NEW(inode) IFS_LINUX_INODE_IS_NEW(inode)
#define IFS_OFS_MKDIR_RETURN IFS_LINUX_MKDIR_RETURN
#define IFS_OFS_MKDIR_FAILURE(error) IFS_LINUX_MKDIR_FAILURE(error)
#define IFS_OFS_MKDIR_SUCCESS IFS_LINUX_MKDIR_SUCCESS

#define IFS_AMIGA_MKDIR_RETURN IFS_OFS_MKDIR_RETURN
#define IFS_AMIGA_BLOCK_VALID(sb, block) \
    (ifs_ofs_data_block_valid( \
        (ifs_ofs_u32)(block), \
        (ifs_ofs_u32)AFFS_SB(sb)->s_reserved, \
        (ifs_ofs_u32)AFFS_SB(sb)->s_partition_size) != 0)
#include "affs_compat.h"
#undef IFS_AMIGA_BLOCK_VALID
#undef IFS_AMIGA_MKDIR_RETURN

#define AFFS_DATA_HEAD(bh) \
    ((struct affs_data_head *)(bh)->b_data)
#define AFFS_DATA(bh) \
    (((struct affs_data_head *)(bh)->b_data)->data)

#define AFFSNAMEMAX IFS_OFS_DOS_NAME_MAX
#define AFFS_MOUNT_SF_OFS 0x0200UL
#define ifs_ofs_chain_budget ifs_amiga_chain_budget

extern const struct file_operations affs_file_operations_ofs;
extern const struct address_space_operations affs_aops_ofs;

#endif
