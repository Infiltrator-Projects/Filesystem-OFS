/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_FS_LINUX_VFS_COMPAT_H
#define INFILTRATOR_FS_LINUX_VFS_COMPAT_H

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/version.h>

/*
 * Keep Linux VFS API-version adaptation in one kernel-safe header.  Each
 * filesystem module compiles these definitions into its own .ko; this is
 * source sharing, not a shared runtime module or private kernel ABI.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define IFS_LINUX_AOPS_WRITE_CONTEXT const struct kiocb *iocb
#define IFS_LINUX_AOPS_WRITE_CONTEXT_ARG iocb
#define IFS_LINUX_INODE_IS_NEW(inode) \
    ((inode_state_read_once(inode) & I_NEW) != 0)
#define IFS_LINUX_MKDIR_RETURN struct dentry *
#define IFS_LINUX_MKDIR_FAILURE(error) ERR_PTR(error)
#define IFS_LINUX_MKDIR_SUCCESS NULL
#else
#define IFS_LINUX_AOPS_WRITE_CONTEXT struct file *file
#define IFS_LINUX_AOPS_WRITE_CONTEXT_ARG file
#define IFS_LINUX_INODE_IS_NEW(inode) (((inode)->i_state & I_NEW) != 0)
#define IFS_LINUX_MKDIR_RETURN int
#define IFS_LINUX_MKDIR_FAILURE(error) (error)
#define IFS_LINUX_MKDIR_SUCCESS 0
#endif

#endif
