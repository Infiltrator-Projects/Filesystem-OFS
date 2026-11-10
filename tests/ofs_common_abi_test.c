/* SPDX-License-Identifier: GPL-3.0-or-later
 * Validate the sole Common V2 filesystem contract and reject V1 exports.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    IfsFsModuleApiEntryV2 entry=NULL;
    const IfsFsModuleApiV2 *api=NULL;
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *volume;
    void *so, *symbol;
    if(argc!=2)return 2;
    so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!so){fputs(dlerror(),stderr);return 1;}
    symbol=dlsym(so,IFS_FS_COMMON_EXPORT_V2);
    if(sizeof(entry)!=sizeof(symbol))return 1;
    memcpy(&entry,&symbol,sizeof(entry));
    api=entry?entry():NULL;
    core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    volume=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!ifs_fs_api_v2_valid(api,"ofs") ||
       !core || !core->probe || !core->mount || !core->unmount ||
       !core->stat || !core->directory_entry || !core->read ||
       !core->write || !core->create || !core->remove ||
       !core->rename || !core->truncate ||
       !volume || !volume->query || !volume->format ||
       !volume->sync || !volume->check ||
       ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG) ||
       dlsym(so,"infiltrator_filesystem_api_v1") ||
       (api->runtime_profiles & IFS_FS_PROFILE_KERNEL)) {
        fputs("OFS strict Common V2 contract rejected\n",stderr);
        dlclose(so);return 1;
    }
    puts("OFS-STRICT-COMMON-V2-CONTRACT-PASS");
    dlclose(so);return 0;
}
