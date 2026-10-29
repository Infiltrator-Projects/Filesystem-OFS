# DOS\\0 — feature-completion and independent acceptance matrix

**29 October 2026 shared-module update:** the independent OFS.so V3 and generic
Boot V1 implementation and its current software evidence are tracked in
[SO completion and qualification](SO_COMPLETION.md). The historical platform
assessment below is retained; Linux, physical-media and original-Amiga gates
are not closed by the separate SO/Boot release.


**Scope:** Authentic original AmigaDOS Original File System (`DOS\\0`, DOSType `0x444F5300`), **not** international `DOS\\2`, directory-cache `DOS\\4`, LNFS `DOS\\6`, FFS or muFS. Canonical disk format: [`native/filesystems/ofs/OFS_SPECIFICATION.md`](../native/filesystems/ofs/OFS_SPECIFICATION.md).

**Status (24 October 2026): NOT FEATURE COMPLETE.** A passing CI build, an empty formatted image, a successful synthetic self-check or even a mounted 32-MiB volume does not complete the format. Green checks certify only exercised cases. This is a mandatory closure list, not a declaration that any unchecked row has passed.

| DOS0 qualification gate | Current demonstrated state | Required acceptance evidence |
| --- | --- | --- |
| Image format, boot/root/checksum/bitmap | 512..32768-byte disposable images and sparse >1-TiB image geometry tested | Independent AmigaDOS/ADFlib comparisons over eligible block sizes; floppy/hardfile/RDB differences |
| Populated volume verify | Previous formatter only recognized empty images; branch adds a traversal of conventional directory hash entries, reversed file-extension pointers, per-block OFS data checksums and bitmap cross-check | Test on independently produced populated nested data, link and corrupt-media fixtures; reject cycles, lost allocations and checksum mismatch; support hardlink/corruption edge cases |
| DOS0 names | ASCII folding and 30-byte rules are implemented | 1-, 30-byte, ASCII mixed-case names, collisions, high-byte DOS0 semantics, rename/unlink/case-insensitive lookup |
| File data and large files | QEMU static-ELF execution, ordinary create/read/mount | QEMU binary payloads crossing 488-byte data, 72-pointer header, multiple T_LIST extensions, 4-GiB file boundary; append/truncate/interleaved dirty write and direct-I/O behavior where advertised |
| Allocation/bitmap and recovery | QEMU read-only 120-GiB statfs and batched bitmap mount; crash-after-sync recovery | Near-full disk ENOSPC, bitmap tail/extension corruption, crash points during allocations and deletion, repeated mount/unmount, fsck/recovery compatibility |
| Directory/links/protection/times | Basic QEMU mkdir, cross-directory rename, symlink, hardlink, desktop ownership tested | Link promotion, parent cycles, rename exchange/replace, long path, permission bits, exact Amiga dates/protection semantics and symlink round-trips |
| Linux 512-byte OFS | Mount/read/write/execute tested on disposable QEMU image | Repeated stress, independent media interoperability, negative corruption, kernel-matrix qualification |
| Linux 1024..4096-byte OFS | Linux chooses candidates only up to PAGE_SIZE; cross-size format tests only | Native Linux read/write with actual large-block images, extension and checksum tests, no page/buffer assumption |
| Linux 8192..32768-byte OFS | **Unsupported** by kernel buffer-head path above PAGE_SIZE | Implement an OFS-native metadata/block I/O layer independent of PAGE_SIZE; genuine read/write qualification for all three sizes |
| Unsigned block addresses / 2-TiB arithmetic | Core primitives and image formatter use unsigned keys; Linux mount and buffer paths retain INT_MAX limits | Remove signed block keys and narrowing end to end, bounds-safe reads/writes at >2^31 blocks, independent >1-TiB qualification |
| EFI read-only loader | DOS0 512-byte reader works on qualified synthetic cases; UEFI Boot relies on integration | All supported block sizes, large file and fragmented kernel/initramfs, original device read boundaries, deterministic EOF/partial I/O handling, actual boot/root-handoff test |
| Physical formatter | Exclusive/confirmed unmounted partition, 512-byte blocks, <=INT_MAX keys | No widening until matching Linux+EFI readers/writers pass and independently created full-size image and physical-media tests are green |
| Windows OFS | **Not implemented** (Windows folder is README only) | Native Windows driver or explicitly scoped-out Windows; independently qualify supported I/O and document deployment |
| Cross-OS interoperability | Defragmenter-owned formatter matches our empty DOS0 image; limited media | Write in one OS, read/write/remount in an independent original Amiga handler, check on-disk equality and metadata semantics |
| Documentation, source boundaries, release | Canonical OFS spec in `main`; compliance audit and longname work tracked separately | Specific gates checked by CI; reproducible source, artifacts and logs; reviewed release/tag; clean main post-merge workflows |

## Immediate engineering sequence

1. **Exercise populated DOS0 end-to-end.** The development branch extends the image checker and adds a QEMU extended-namespace regression. Run the full independent `Amiga runtime qualification` workflow, not just Linux unit tests. Investigate any failure; do not delete assertions to make it green.
2. **Qualify the 512-byte DOS0 surface** exhaustively (names, data, hash chains, file-extension rollover, hard/soft links, rename, timestamps, permissions, ENOSPC, bitmap and corruption). Add independent fixtures to prevent checker/writer bugs from cancelling each other.
3. **Fix unsigned address and page-size limitations in Linux**, with stress tests across 2^31 keys and the seven documented filesystem block sizes. Do not assume high-capacity sparse test images imply safe physical writes.
4. **Expand the EFI DOS0 reader and Boot integration** to all required filesystem sizes and actual EFI kernel/initramfs streaming, with progress, failure attribution and boot fallback.
5. **Prove original-Amiga interoperability and decide the Windows release scope** explicitly before claiming universal DOS0 feature completeness.

## Honest sign-off requirements

`DOS\\0 FEATURE COMPLETE` requires every in-scope row to have reproducible green independent qualification evidence, including at least one original Amiga-format producer/consumer. Windows may be separately declared out of the Linux/EFI release scope, but cannot be silently presented as implemented. Changes for DOS2/4/6 are different tasks. Retain the original fixed-offset and on-disk-format semantics.

Source implementation limitations must be documented in this matrix and `docs/OFS_SPECIFICATION_COMPLIANCE.md`; the specification itself must remain independent of the current implementation.

**Source and tracking:** [Full OFS compliance issue #27](https://github.com/Infiltrator-Projects/Filesystems/issues/27); [DOS6 independent issue #29](https://github.com/Infiltrator-Projects/Filesystems/issues/29).
