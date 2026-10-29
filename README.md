# Filesystem-OFS

**Source owner:** Independent AmigaDOS Old File System engine for InfiltratorOS.

This repository contains the actual OFS implementation imported from [Filesystems](https://github.com/Infiltrator-Projects/Filesystems/tree/7473534b1afbd1e9b14f8ab9ae0769fc6be2126e/native/filesystems/ofs) at source commit `7473534b1afbd1e9b14f8ab9ae0769fc6be2126e` on 29 October 2026. Source paths, copyright notices, Linux VFS adapter, UEFI driver, and original test code are preserved.

- `native/filesystems/ofs/core/`: canonical on-disk formats, checksums and bounds primitives.
- `native/filesystems/ofs/module/`: freestanding `OFS.so` ABI V1 reader and V2 bounded existing-file writer with checksum and persistence checks.
- `native/filesystems/ofs/linux/`: actual OFS Linux VFS driver source. Not an independent Linux-kernel build release yet.
- `native/filesystems/ofs/efi/`: actual read-only GNU-EFI filesystem driver, using the same OFS core.
- `tests/`: original OFS test suite and independent/negative fixture sources.

Build and test the standalone shared object using `make test` on a Linux development host. Build an EFI driver with `make efi` on an x86-64 GNU-EFI host. Neither command installs a driver, accesses a physical disk, or changes an ESP/NVRAM entry.

**Correctness status:** V1/V2 file I/O, checksum and corruption tests cover a restricted subset of OFS. Full directory mutation, allocation and formatting in `OFS.so`, all seven logical block sizes, signed-address limits in Linux, crash recovery and full BOOT/kernel universal-SO loading remain unfinished. The Linux and firmware adapters are transitional compatibility implementations; do not delete them before genuine loader equivalence is demonstrated.

OFS is distinct from Amiga FFS, SFS, SFS2 and PFS3. Do not copy unrelated filesystem sources into this independent repository. Read `docs/OFS_DOS0_COMPLETION_MATRIX.md` before calling this driver feature-complete.
