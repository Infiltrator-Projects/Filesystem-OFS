#include "ofs_core.h"
#include "ofs_disk_layout.h"
#include <stddef.h>
#include <stdio.h>
static int fail(const char *m){fprintf(stderr,"ofs core test: %s\n",m);return 1;}
int main(void){ifs_ofs_u32 f=0;
if(ifs_ofs_classify_dostype(IFS_OFS_DOS_OFS,&f)||f!=0)return fail("OFS");
if(ifs_ofs_classify_dostype(IFS_OFS_DOS_INTL_OFS,&f)||f!=IFS_OFS_VARIANT_INTL)return fail("INTL OFS");
if(ifs_ofs_classify_dostype(IFS_OFS_DOS_DC_OFS,&f)||f!=(IFS_OFS_VARIANT_INTL|IFS_OFS_VARIANT_DIRCACHE))return fail("DC OFS");
if(ifs_ofs_classify_dostype(IFS_OFS_MUFS_DC_OFS,&f)||f!=(IFS_OFS_VARIANT_MUFS|IFS_OFS_VARIANT_INTL|IFS_OFS_VARIANT_DIRCACHE))return fail("MUFS DC OFS");
/* DOS6 requires a different nonroot tail and can never silently be
 * interpreted with the old 30-byte affs_tail.  Existing Linux/EFI readers
 * deliberately reject it until their namespace mapping is upgraded. */
if(ifs_ofs_classify_dostype(0x444F5306U,&f)==0)return fail("LNFS incorrectly classified as classic OFS");
if(sizeof(struct ifs_ofs_lnfs_tail)!=200U)return fail("LNFS tail width");
if(offsetof(struct ifs_ofs_lnfs_tail,name_and_comment)!=16U)
    return fail("LNFS NaC tail offset");
if(offsetof(struct ifs_ofs_lnfs_tail,comment_block)!=128U)
    return fail("LNFS comment key offset");
if(offsetof(struct ifs_ofs_lnfs_tail,change)!=140U)
    return fail("LNFS date offset");
if(offsetof(struct ifs_ofs_lnfs_tail,hash_chain)!=184U)
    return fail("LNFS hash-chain offset");
if(sizeof(struct ifs_ofs_lnfs_comment_head)!=104U)
    return fail("LNFS comment header width");
if(offsetof(struct ifs_ofs_lnfs_comment_head,comment)!=24U)
    return fail("LNFS comment block data offset");
if(ifs_ofs_classify_dostype(0x444F5301U,&f)==0)return fail("FFS accepted");
return 0;}
