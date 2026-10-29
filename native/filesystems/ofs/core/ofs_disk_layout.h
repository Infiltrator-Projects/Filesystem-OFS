#ifndef INFILTRATOR_OFS_CORE_DISK_LAYOUT_H
#define INFILTRATOR_OFS_CORE_DISK_LAYOUT_H

/*
 * Canonical OFS on-disk layout.
 *
 * This header contains filesystem-format structures only. It deliberately
 * avoids Linux VFS objects so the same layout contract can be consumed by
 * Linux, Windows and userspace qualification code.
 */
#if defined(__KERNEL__)
#include <linux/types.h>
typedef __be16 ifs_ofs_disk_be16;
typedef __be32 ifs_ofs_disk_be32;
typedef u8 ifs_ofs_disk_u8;
#elif defined(IFS_OFS_WINDOWS_KERNEL)
typedef unsigned short ifs_ofs_disk_be16;
typedef unsigned int ifs_ofs_disk_be32;
typedef unsigned char ifs_ofs_disk_u8;
#else
#include <stdint.h>
typedef uint16_t ifs_ofs_disk_be16;
typedef uint32_t ifs_ofs_disk_be32;
typedef uint8_t ifs_ofs_disk_u8;
#endif

#define FS_OFS          0x444f5300U
#define FS_FFS          0x444f5301U
#define FS_INTLOFS      0x444f5302U
#define FS_INTLFFS      0x444f5303U
#define FS_DCOFS        0x444f5304U
#define FS_DCFFS        0x444f5305U

#define MUFS_FS         0x6d754653U
#define MUFS_OFS        0x6d754600U
#define MUFS_FFS        0x6d754601U
#define MUFS_INTLOFS    0x6d754602U
#define MUFS_INTLFFS    0x6d754603U
#define MUFS_DCOFS      0x6d754604U
#define MUFS_DCFFS      0x6d754605U

#define T_SHORT         2
#define T_LIST          16
#define T_DATA          8

#define ST_LINKFILE     (-4)
#define ST_FILE         (-3)
#define ST_ROOT         1
#define ST_USERDIR      2
#define ST_SOFTLINK     3
#define ST_LINKDIR      4

#define AFFS_ROOT_BMAPS 25U
#define AFFS_EPOCH_DELTA ((8LL * 365LL + 2LL) * 86400LL)

struct affs_date {
    ifs_ofs_disk_be32 days;
    ifs_ofs_disk_be32 mins;
    ifs_ofs_disk_be32 ticks;
};

struct affs_short_date {
    ifs_ofs_disk_be16 days;
    ifs_ofs_disk_be16 mins;
    ifs_ofs_disk_be16 ticks;
};

struct affs_root_head {
    ifs_ofs_disk_be32 ptype;
    ifs_ofs_disk_be32 reserved0;
    ifs_ofs_disk_be32 reserved1;
    ifs_ofs_disk_be32 hash_size;
    ifs_ofs_disk_be32 reserved2;
    ifs_ofs_disk_be32 checksum;
    ifs_ofs_disk_be32 table[];
};

struct affs_root_tail {
    ifs_ofs_disk_be32 bitmap_valid;
    ifs_ofs_disk_be32 bitmap_blocks[AFFS_ROOT_BMAPS];
    ifs_ofs_disk_be32 bitmap_extension;
    struct affs_date root_change;
    ifs_ofs_disk_u8 disk_name[32];
    ifs_ofs_disk_be32 reserved0;
    ifs_ofs_disk_be32 reserved1;
    struct affs_date disk_change;
    struct affs_date disk_create;
    ifs_ofs_disk_be32 reserved2;
    ifs_ofs_disk_be32 reserved3;
    ifs_ofs_disk_be32 directory_cache;
    ifs_ofs_disk_be32 stype;
};

/*
 * Compatibility field aliases keep the Linux adapter readable while the
 * canonical core is migrated away from direct structure access.
 */
#define bm_flag bitmap_valid
#define bm_blk bitmap_blocks
#define bm_ext bitmap_extension
#define dcache directory_cache

struct affs_head {
    ifs_ofs_disk_be32 ptype;
    ifs_ofs_disk_be32 key;
    ifs_ofs_disk_be32 block_count;
    ifs_ofs_disk_be32 reserved;
    ifs_ofs_disk_be32 first_data;
    ifs_ofs_disk_be32 checksum;
    ifs_ofs_disk_be32 table[];
};

struct affs_tail {
    ifs_ofs_disk_be32 reserved0;
    ifs_ofs_disk_be16 uid;
    ifs_ofs_disk_be16 gid;
    ifs_ofs_disk_be32 protect;
    ifs_ofs_disk_be32 size;
    ifs_ofs_disk_u8 comment[92];
    struct affs_date change;
    ifs_ofs_disk_u8 name[32];
    ifs_ofs_disk_be32 reserved1;
    ifs_ofs_disk_be32 original;
    ifs_ofs_disk_be32 link_chain;
    ifs_ofs_disk_be32 reserved2[5];
    ifs_ofs_disk_be32 hash_chain;
    ifs_ofs_disk_be32 parent;
    ifs_ofs_disk_be32 extension;
    ifs_ofs_disk_be32 stype;
};

/* Long Name OFS (DOS\\6): this tail replaces the classic affs_tail.
 * Its length is exactly 200 bytes, anchored to the END of each header.
 * NaC (Name and Comment) is 112 bytes: [nameLen][name][commentLen][comment].
 * On-disk encoding and offset source: Olaf Barthel's LNFS structures,
 * https://wiki.amigaos.net/wiki/DCFS_and_LNFS_Low_Level_Data_Structures
 *
 * Do not use affs_tail for DOS6: its classic name field has moved. */
struct ifs_ofs_lnfs_tail {
    ifs_ofs_disk_be32 reserved0;
    ifs_ofs_disk_be16 uid;
    ifs_ofs_disk_be16 gid;
    ifs_ofs_disk_be32 protect;
    ifs_ofs_disk_be32 size;
    ifs_ofs_disk_u8 name_and_comment[112];
    ifs_ofs_disk_be32 comment_block;
    ifs_ofs_disk_be32 reserved1[2];
    struct affs_date change;
    ifs_ofs_disk_be32 reserved2[2];
    ifs_ofs_disk_be32 link_chain;
    ifs_ofs_disk_be32 reserved3[5];
    ifs_ofs_disk_be32 hash_chain;
    ifs_ofs_disk_be32 parent;
    ifs_ofs_disk_be32 extension;
    ifs_ofs_disk_be32 stype;
};

/* LNFS external comment: type 64, block checksum at byte 20,
 * comment string at byte 24; remainder of filesystem block reserved. */
#define IFS_OFS_T_COMMENT 64U
struct ifs_ofs_lnfs_comment_head {
    ifs_ofs_disk_be32 ptype;
    ifs_ofs_disk_be32 key;
    ifs_ofs_disk_be32 header_key;
    ifs_ofs_disk_be32 reserved0[2];
    ifs_ofs_disk_be32 checksum;
    ifs_ofs_disk_u8 comment[80];
};

/* LNFS root reuses the classic root tail, assigning new purposes to
 * relative longwords -11 and -4. Use B-44 and B-16 respectively. */
#define IFS_OFS_LNFS_ROOT_USED_BLOCKS_FROM_END 44U
#define IFS_OFS_LNFS_ROOT_DOSTYPE_FROM_END 16U
#define IFS_OFS_LNFS_NAC_FROM_END 184U
#define IFS_OFS_LNFS_NAC_BYTES 112U
#define IFS_OFS_LNFS_COMMENT_KEY_FROM_END 72U
#define IFS_OFS_LNFS_DATE_FROM_END 60U

struct slink_front {
    ifs_ofs_disk_be32 ptype;
    ifs_ofs_disk_be32 key;
    ifs_ofs_disk_be32 reserved[3];
    ifs_ofs_disk_be32 checksum;
    ifs_ofs_disk_u8 symname[];
};

struct affs_data_head {
    ifs_ofs_disk_be32 ptype;
    ifs_ofs_disk_be32 key;
    ifs_ofs_disk_be32 sequence;
    ifs_ofs_disk_be32 size;
    ifs_ofs_disk_be32 next;
    ifs_ofs_disk_be32 checksum;
    ifs_ofs_disk_u8 data[];
};

#define FIBF_OTR_READ       0x8000U
#define FIBF_OTR_WRITE      0x4000U
#define FIBF_OTR_EXECUTE    0x2000U
#define FIBF_OTR_DELETE     0x1000U
#define FIBF_GRP_READ       0x0800U
#define FIBF_GRP_WRITE      0x0400U
#define FIBF_GRP_EXECUTE    0x0200U
#define FIBF_GRP_DELETE     0x0100U
#define FIBF_HIDDEN         0x0080U
#define FIBF_SCRIPT         0x0040U
#define FIBF_PURE           0x0020U
#define FIBF_ARCHIVED       0x0010U
#define FIBF_NOREAD         0x0008U
#define FIBF_NOWRITE        0x0004U
#define FIBF_NOEXECUTE      0x0002U
#define FIBF_NODELETE       0x0001U

#define FIBF_OWNER          0x000fU
#define FIBF_MASK           0xee0eU

#endif
