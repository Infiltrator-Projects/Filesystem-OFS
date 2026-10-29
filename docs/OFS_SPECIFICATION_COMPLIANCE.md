# OFS format completeness and implementation qualification

**DOS0 implementation evidence matrix:** [OFS_DOS0_COMPLETION_MATRIX.md](OFS_DOS0_COMPLETION_MATRIX.md) records DOS0 verification and remaining platform qualification requirements.

**DOS0-specific acceptance contract:** [native/filesystems/ofs/DOS0_COMPLETION.md](../native/filesystems/ofs/DOS0_COMPLETION.md) tracks feature completion of the original DOS0 format independently of the other OFS variants.

**0.5.79 candidate update:** the Linux OFS signed `int s_partition_size`, signed buffer-head block arguments, and `INT_MAX` root-geometry gates are being replaced with unsigned 32-bit filesystem key handling in this branch. The >1-TiB runtime and independent Amiga interoperability gates stay **open** until proven; do not assume this change alone certifies high-capacity mounting.

Status: **open / incomplete**, audited 24 October 2026.

## Authority and separation

AmigaDOS **OFS** includes DOS\\0, DOS\\2, DOS\\4 and DOS\\6. FFS is a distinct on-media format family whose identifiers are specified in [its own FFS specification](../native/filesystems/ffs/FFS_SPECIFICATION.md). Linux
`affs` is a combined adapter, **not** proof that the OFS and FFS data
layouts or feature limits are interchangeable. The Linux kernel explicitly
distinguishes the DOS types, and documents a 512–32768-byte format block-size
range but a *narrower* 512–4096-byte range in its own adapter:
https://www.kernel.org/doc/html/v6.7/filesystems/affs.html

Do not import FFS raw data-block semantics into OFS: OFS file data blocks
carry a 24-byte header and payload `block_size - 24`, while FFS data blocks
contain raw payload. Each format owns its parser, allocation, write and
recovery invariants. Shared primitive code is allowed only when its
semantics demonstrably agree.

The 32-bit block-reference representation has a **theoretical** maximum of
`(2^32 - 1) * block_size` bytes; at 512 bytes this is just under **2 TiB**,
and at 32768 bytes just under **128 TiB**. These are arithmetic upper
bounds, **not** claims of original AmigaOS device-driver interoperability,
end-to-end file size, filesystem integrity, or root suitability.

The AmigaOS wiki describes 512–32768-byte FFS block sizes and partition
limits depending on block size. It does **not** qualify original DOS\\0 OFS
on 128-TiB physical media:
https://wiki.amigaos.net/wiki/UserDoc:AmigaOS_File_Systems

## Audited implementation matrix

| Surface | Before this audit | Current qualification boundary |
| --- | --- | --- |
| OFS on-disk primitives | unsigned 32-bit block pointers; block-size-dependent checksum/bitmap primitives | portable math; independent format tests needed |
| OFS image formatter | 512-byte block hardcode; INT32_MAX block limit | experimental image creation and verification for 512..32768 blocks, plus sparse >1-TiB case; tests do not independently prove all historical implementations |
| OFS physical formatter | guarded 512-byte sectors, 512-byte filesystem blocks | **still refuses >INT32_MAX 512-byte blocks** until independently qualified Linux + EFI readers |
| OFS Linux `ofs.ko` | `s_partition_size` signed `int`; mount rejects `>INT_MAX`; relies on `buffer_head`/PAGE_SIZE | **not qualified** >1 TiB @512 or >PAGE_SIZE |
| OFS EFI `OFS_x64.efi` | fixed 512-byte parser, DOS\\0 only, rejects `>0x7fffffff` blocks | **not qualified** for larger filesystem blocks or >1 TiB @512 |
| FFS `ffs.ko` | separate source/core | independent FFS review; no OFS limits inferred from FFS |

The 1-TiB boundary in EFI/Linux is an **implementation limitation**, not
an authoritative OFS limit. The image formatter must not widen destructive
physical formatting before the kernel and EFI reader can read/write the same
geometry correctly. **Successful formatting does not demonstrate a bootable
filesystem.**

## 0.5.77 remediation candidate (not released)

Documented **before** the source changes in
[OFS_CORRECTNESS_REMEDIATION_2026-10-24.md](OFS_CORRECTNESS_REMEDIATION_2026-10-24.md)
at commit `0d0e0199fea8754ef77d87fdfe3637819b1c52d3`.

| Candidate | Implementation | Qualification |
| --- | --- | --- |
| Odd-volume root placement | Shared `ifs_ofs_root_block(reserved, blocks)` used by image formatter/verifier, EFI reader and Linux mount; image creator accepts an explicitly requested total block count for disposable tests | Seven-size odd/even image tests; signed-boundary and UINT32_MAX metadata tests. **CI pending**, independent real Amiga device testing still required |
| Read-only DOS variants in EFI | 512-byte EFI reader now uses international name folding for DOS\\2 and DOS\\4; DOS\\4 uses hash-chain reads and does not write to or depend on optional directory-cache blocks | Synthetic DOS\\2 Latin-1, DOS\\4 hash and FFS rejection tests; still **not qualified** for 1024..32768-byte OFS filesystem blocks |
| Linux executable access | Custom OFS folio-backed read implementation and existing `generic_file_mmap` permit a controlled removal of automatic `SB_NOEXEC` on the candidate branch | **Not accepted until** QEMU proves direct execution of a copied/checksummed static ELF on disposable OFS and reports `AMIGA-OFS-EXEC-PASS`. Does not establish actual OFS root boot |
| Unchanged safety gates | Physical formatter remains capped to 512-byte filesystem blocks and signed INT_MAX block counts; Linux high-address and >PAGE_SIZE work remains open | Do **not** claim large-block boot, physical >1 TiB support, or full specification conformance |

A passing test of one row closes only that row. Full sign-off also requires
independent layout comparison, error recovery, filesystem root handoff, and
EFI block-size support. Experimental test artifacts cannot be installed as
fully qualified providers on physical media.

## Completion gates (all required)

1. Validate authentic OFS root/tail/hash/bitmap layouts for *each* power-of-two
   block size 512, 1024, 2048, 4096, 8192, 16384 and 32768. Test damaged
   checksums, incorrect geometry, bitmap extensions and directory entries.
2. Independently read the resulting images with at least one trusted
   implementation at its supported sizes, and document true interoperability
   gaps rather than silently rewriting the media layout.
3. Remove signed block-address storage and narrowing throughout `ofs.ko`;
   add overflow-safe geometry, block/sector offset and allocation tests across
   2^31 and near 2^32, including mounts and writes on disposable loop media.
4. Implement EFI block-size discovery and full-block 512..32768 reads, while
   preserving low-address DMA requirements, read-only binding, bounded
   allocation and error attribution. Test real large files, not a tiny fixture.
5. Qualify >1-TiB sparse fixtures without confusing sparse test-media support
   with durable physical storage qualification or the OFS 4-GiB file-size
   boundary.
6. Retest OFS and FFS independently to reject cross-format DOS types and
   preserve OFS's per-data-block headers/checksums.
7. Only then widen physical partition formatting and resume the Dell NVMe
   boot-source experiment, retaining Boot0004 GRUB/Windows recovery. Never
   touch the user's NVMe test partition during source qualification.

**Release rule:** Do not label OFS full-specification, 2-TiB, 32-KiB
Linux-mountable, or EFI-boot-qualified until the corresponding tests have
passed. A green compilation or an image-only formatter test is not enough.
