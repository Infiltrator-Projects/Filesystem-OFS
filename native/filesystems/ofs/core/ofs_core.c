#include "ofs_core.h"
int ifs_ofs_classify_dostype(
    const ifs_ofs_u32 dostype, ifs_ofs_u32 *const variant_flags)
{
    if (variant_flags == 0)
        return -1;

    switch (dostype) {
    case IFS_OFS_DOS_OFS:
        *variant_flags = 0U;
        return 0;
    case IFS_OFS_DOS_INTL_OFS:
        *variant_flags = IFS_OFS_VARIANT_INTL;
        return 0;
    case IFS_OFS_DOS_DC_OFS:
        *variant_flags =
            IFS_OFS_VARIANT_INTL | IFS_OFS_VARIANT_DIRCACHE;
        return 0;
    case IFS_OFS_MUFS_OFS:
        *variant_flags = IFS_OFS_VARIANT_MUFS;
        return 0;
    case IFS_OFS_MUFS_INTL_OFS:
        *variant_flags =
            IFS_OFS_VARIANT_MUFS | IFS_OFS_VARIANT_INTL;
        return 0;
    case IFS_OFS_MUFS_DC_OFS:
        *variant_flags =
            IFS_OFS_VARIANT_MUFS |
            IFS_OFS_VARIANT_INTL |
            IFS_OFS_VARIANT_DIRCACHE;
        return 0;
    default:
        *variant_flags = 0U;
        return -1;
    }
}
