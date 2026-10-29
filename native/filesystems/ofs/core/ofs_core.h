#ifndef INFILTRATOR_OFS_CORE_H
#define INFILTRATOR_OFS_CORE_H
#if defined(__KERNEL__)
#include <linux/types.h>
typedef u32 ifs_ofs_u32;
#else
#include <stdint.h>
typedef uint32_t ifs_ofs_u32;
#endif
#define IFS_OFS_DOS_OFS       0x444F5300U
#define IFS_OFS_DOS_INTL_OFS  0x444F5302U
#define IFS_OFS_DOS_DC_OFS    0x444F5304U
#define IFS_OFS_MUFS_OFS      0x6d754600U
#define IFS_OFS_MUFS_INTL_OFS 0x6d754602U
#define IFS_OFS_MUFS_DC_OFS   0x6d754604U
#define IFS_OFS_VARIANT_MUFS      0x01U
#define IFS_OFS_VARIANT_INTL      0x02U
#define IFS_OFS_VARIANT_DIRCACHE  0x04U
int ifs_ofs_classify_dostype(ifs_ofs_u32 dostype, ifs_ofs_u32 *variant_flags);
#endif
