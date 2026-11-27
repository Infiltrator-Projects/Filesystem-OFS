# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(OFS_firmware C)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "BOOT firmware profile requires AMD64")
endif()
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
  message(FATAL_ERROR "Pass the pinned canonical Common V2 include directory")
endif()
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(FS "${ROOT}/native/filesystems/ofs")
add_library(firmware MODULE
  "${FS}/module/ofs_common.c"
  "${FS}/module/ofs_memory.c"
  "${FS}/core/ofs_core.c"
  "${FS}/core/ofs_primitives.c"
  "${FS}/core/ofs_volume.c"
  "${FS}/core/ofs_nodes.c"
  "${FS}/core/ofs_files.c"
  "${FS}/core/ofs_namespace.c"
  "${FS}/core/ofs_check.c"
  "${FS}/core/ofs_strings.c"
  "${FS}/core/ofs_cache.c"
)
set_target_properties(firmware PROPERTIES PREFIX "" SUFFIX ".so" OUTPUT_NAME "OFS" C_STANDARD 11 C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden)
target_include_directories(firmware PRIVATE "${COMMON_INCLUDE_DIR}" "${FS}/module" "${FS}/core" "${FS}/efi" "${ROOT}/native/include")

target_compile_options(firmware PRIVATE -O2 -march=x86-64 -mtune=generic -mno-red-zone -mgeneral-regs-only -fcf-protection=none -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(firmware PRIVATE -nostdlib -Wl,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now,-z,nopack-relative-relocs,-z,max-page-size=4096 )
