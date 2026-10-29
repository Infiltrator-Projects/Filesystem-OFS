# DOS\0 OFS feature-completion contract

**Status: engineering qualification checklist — NOT a feature-complete declaration.**

**Current implementation evidence and outstanding tests:** [DOS0 completion evidence matrix](../../../docs/OFS_DOS0_COMPLETION_MATRIX.md); this file remains the authoritative A–I acceptance contract. Combined regression PR includes signed-address fixes, populated-volume verification, ELF remount and namespace tests.
  
**Canonical on-disk reference:** [OFS_SPECIFICATION.md](OFS_SPECIFICATION.md).  
**Scope:** original AmigaDOS Old File System DOSType `0x444F5300` / `DOS\0` only. Do not let international DOS2, directory-cache DOS4, long-name DOS6 or FFS semantics delay, contaminate or mask DOS0 qualification.

This document records **what must work** and the proof required before anyone describes DOS0 as feature-complete. It is not permission to assume the corresponding code works. Every checklist item needs a versioned independent fixture or on-device regression and linked passing CI evidence; a successful build or formatter self-verification is insufficient.

## DOS0 contract and authoritative boundaries

- Every regular-file data block is `T_DATA=8`, BE32 owner, 1-based sequence, actual payload length, next-data key and checksum (24-byte header), leaving **B-24** bytes of payload. FFS raw data blocks are **forbidden**.
- Correct boot area, reserved blocks, root middle location including odd-volume geometry, end-relative metadata fields, negative-sum checksums, hash buckets/name folding, bitmap polarity, bitmap extension chain, file header/extension reversed pointer arrays, parent/hash chains and DateStamps.
- All seven canonical logical OFS block sizes (`512,1024,2048,4096,8192,16384,32768`) and all **unsigned 32-bit** partition-relative block keys are a format requirement, **not** a promise that a particular disk, host kernel or firmware can access those ranges. Every implementation must explicitly qualify or reject its supported I/O block geometries and address widths; no silent signed-31-bit aliasing.
- Maximum per-file size is the classic 32-bit byte length, independent of the volume geometry. Path/name rules are classic 30-byte DOS0, not international or long-name mappings.

## Acceptance matrix — mark completed only with evidence

| Gate | Must demonstrate | Status at creation |
| --- | --- | --- |
| A — independent on-disk format | Exact boot/root/bitmap/dir/file/data/ext bytes, checksums, odd/even roots against authentic Amiga-compatible images | **Open**: formatter self-verification and some synthetic fixtures exist |
| B — formatter | All seven B sizes, short/long bitmap chains, root/bitmap collision avoidance, partition-unique ownership, full/near-full, allocation bit coverage, verifier rejecting corrupt images | **Partial**: disposable images tested, real media more restrictive |
| C — read-only Linux | Read/mmap/seek/readdir/statfs, large and tiny files, checksums, bounds, directory collisions, links, removable media errors, unsigned high block keys | **Partial**: QEMU tests pass; unsigned high-key and large B not qualified |
| D — writable Linux | Create/mkdir/write/append/truncate/rename/unlink/rmdir/link/symlink/chmod/chown/timestamps, extension and bitmap maintenance, overflow/ENOSPC/short I/O, crash/remount recovery | **Partial**: QEMU tests cover subset; independent lifetime and adversarial coverage needed |
| E — OFS EFI driver | Actual UEFI driver loads/binds; read-only DOS0 lookup/readdir/open/read/seek from all qualified B sizes, BE/checksum, bounded cycle rejection, DMA and >31-bit addressing, no duplicate files | **Partial**: read-only 512-byte reader and OVMF test |
| F — full Linux root boot | Firmware filesystem driver + Boot manager + exact kernel/initramfs bytes + kernel `rootfstype=ofs` handoff + mounted writable root, reboot; repeated on target hardware | **Open**: direct copied ELF execution proven in QEMU, not full target root boot |
| G — physical format safety | Explicit partition-only exclusive claim, geometric detection, human-confirmed selection, read-back, unmount protection, no disk/NVMe/ESP/NVRAM collateral damage | **Partial**: existing guard intentionally restricts 512B and <=INT32_MAX |
| H — interoperability/recovery | Independent Amiga DOS0 writer and reader, fixture SHA256 provenance, corrupt checksums, links, bitmap mismatches, interruption/power loss, no silent auto-repair | **Open** |
| I — packaging and release | Linux/EFI artifacts trace to same commit, all required CI green on head and main, package repo published/available to APT, target-system install and rollback, explicit supported profiles | **Open for full DOS0** |

## Priorities

1. Audit and remove **artificial, unjustified signed 31-bit block limits** from the Linux DOS0 implementation (including the public block helpers and mount-option types); keep destructive device-format limits until real EFI/Linux read/write has passed.
2. Implement/test EFI access at 512 through 32768-byte **filesystem** blocks and correct logical-sector translation, with buffers and DMA aligned to firmware constraints.
3. Expand Linux support beyond the host's one-PAGE_SIZE buffer_head limit using an actually tested multi-page block I/O strategy, **without** claiming that arbitrary systems can directly mount 32KiB as a VFS block size.
4. Qualify every DOS0 namespace and recovery operation on independent fixtures and stress media; add end-to-end Linux root boot.
5. Only then lift physical-formatter geometry restrictions and declare DOS0 complete, supported and released.

### Immediate implementation notes

- The Linux superblock currently uses `int s_partition_size`, and root discovery checks `INT_MAX`. The public `affs_bread` and `affs_getblk` block arguments are `int`; these must not reinterpret unsigned block keys as negative. A change only to the initial size check is **not a fix**.
- User `root=` mount arguments must not silently truncate unsigned values. Retain a distinct **not specified** state, use checked parsing, and reject anything exceeding `UINT32_MAX`.
- Every size change requires overflow-safe bitmap multiplication and remaining-block arithmetic. Existing tests must continue to accept the historical small DOS0 cases.
- An OFS block may exceed PAGE_SIZE on common Linux hosts. The format permits it; the VFS `sb_set_blocksize` contract does not. A separate validated multi-page strategy is needed; expanding the mount-option parser alone would be dishonest.
- Experimental, disposable regular-file image tests never authorize formatting a physical SD card or altering the EFI System Partition.

**Do not mark this document 'complete' until gates A–I all have repeatable evidence.** Keep non-DOS0 variant compatibility work tracked separately.
