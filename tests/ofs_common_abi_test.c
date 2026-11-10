/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <stdio.h>
int main(int argc,char **argv) {
    IfsFsModuleApiEntryV1 entry=NULL;
    IfsFsModuleApiEntryV2 entry_v2=NULL;
    const IfsFsModuleApiV2 *v2=NULL;
    void *so;
    const IfsFsModuleApiV1 *api;
    if(argc!=2)return 2;
    so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!so){fputs(dlerror(),stderr);return 1;}
    *(void **)(&entry)=dlsym(so,IFS_FS_COMMON_EXPORT_V1);
    api=entry?entry():NULL;
    if(!ifs_fs_common_api_valid(api,"ofs") ||
       !(api->capabilities & IFS_FS_CAP_FORMAT) ||
       !(api->capabilities & IFS_FS_CAP_CHECK) ||
       (api->capabilities & (IFS_FS_CAP_RECOVER|IFS_FS_CAP_DEFRAG_MOVE))) {
        fputs("OFS Common V1 descriptor rejected\n",stderr);
        dlclose(so);return 1;
    }
    puts("OFS Common V1 descriptor passed");
    *(void **)(&entry_v2)=dlsym(so,IFS_FS_COMMON_EXPORT_V2);
    v2=entry_v2?entry_v2():NULL;
    if(!ifs_fs_api_v2_valid(v2,"ofs")||
       v2->base_v1!=api ||
       ifs_fs_group_v2(v2,IFS_FS_GROUP_VOLUME)==NULL ||
       !(((const IfsFsVolumeOpsV1 *)ifs_fs_group_v2(v2,IFS_FS_GROUP_VOLUME))
          ->header.capabilities & IFS_FS_VOLUME_QUERY) ||
       !((const IfsFsVolumeOpsV1 *)ifs_fs_group_v2(v2,IFS_FS_GROUP_VOLUME))
          ->query ||
       (v2->runtime_profiles&IFS_FS_PROFILE_KERNEL)) {
        fputs("OFS V2 contract rejected\n",stderr);
        dlclose(so);return 1;
    }
    dlclose(so);return 0;
}
