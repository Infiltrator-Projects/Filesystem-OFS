/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_BOOT_MODULE_H
#define INFILTRATOR_BOOT_MODULE_H
#include <stdint.h>

/* Shared byte contract for an x86-64 freestanding ELF DSO. All calls, including
 * host callbacks, use SysV even when the UEFI consumer uses Microsoft x64.
 * Each loaded image owns one mount; consumers instantiate another image for
 * another volume. No constructors, imports, TLS, red zone or host libc. */
#if defined(__x86_64__) && defined(__GNUC__)
#define IFS_BOOT_CALL __attribute__((sysv_abi))
#else
#define IFS_BOOT_CALL
#endif
#define IFS_BOOT_ABI 1u
#define IFS_BOOT_PROFILE_X64 1u
#define IFS_BOOT_SYMBOL "infiltrator_filesystem_boot_v1"
#define IFS_BOOT_OK 0
#define IFS_BOOT_END 1
#define IFS_BOOT_NOT_FOUND 2
#define IFS_BOOT_INVALID (-1)
#define IFS_BOOT_IO_ERROR (-2)
#define IFS_BOOT_DIRECTORY 0x10u
#define IFS_BOOT_NAME_BYTES 768u

typedef struct {
    char magic[8];                 /* IFSBOOT1, no trailing NUL */
    uint32_t structure_bytes, abi_version;
    uint32_t machine, profile;      /* ELF EM_X86_64 (62), profile 1 */
    char format_id[16], display_name[32];
} IfsBootManifest;

typedef struct {
    void *context;
    uint64_t media_bytes;
    uint32_t block_bytes, structure_bytes;
    int (IFS_BOOT_CALL *read_at)(void *, uint64_t, void *, uint32_t);
} IfsBootIo;

typedef struct {
    uint64_t bytes;
    uint32_t attributes, reserved;
    char name[IFS_BOOT_NAME_BYTES];  /* UTF-8, always NUL terminated */
} IfsBootInfo;

typedef struct {
    uint32_t abi_version, structure_bytes;
    char format_id[16];
    uint32_t file_context_bytes, reserved;
    int (IFS_BOOT_CALL *mount)(const IfsBootIo *);
    int (IFS_BOOT_CALL *unmount)(void);
    /* Caller supplies zeroed, suitably aligned context storage and closes it
     * before reuse. Contexts and their image must outlive EFI-stub reads. */
    int (IFS_BOOT_CALL *open)(const char *, void *, IfsBootInfo *);
    int (IFS_BOOT_CALL *close)(void *);
    int (IFS_BOOT_CALL *read)(void *, uint64_t, void *, uint32_t, uint32_t *);
    /* Index zero rewinds. END is distinct from an inaccessible directory. */
    int (IFS_BOOT_CALL *directory_entry)(void *, uint32_t, IfsBootInfo *);
    int (IFS_BOOT_CALL *volume_label)(char *, uint32_t);
} IfsBootModuleV1;

typedef const IfsBootModuleV1 *(IFS_BOOT_CALL *IfsBootEntryPoint)(void);
#endif
