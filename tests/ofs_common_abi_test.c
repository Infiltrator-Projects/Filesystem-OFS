/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module.h>
#include <dlfcn.h>
#include <stdio.h>
int main(int argc,char **argv) {
    IfsFsModuleApiEntryV1 entry=NULL;
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
    dlclose(so);return 0;
}
