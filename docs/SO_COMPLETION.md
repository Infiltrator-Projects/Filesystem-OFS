# OFS shared-module implementation and qualification

This release implements the **shared module and Boot consumer** scope.
The authoritative disk format remains `native/filesystems/ofs/OFS_SPECIFICATION.md`.
The original DOS0 A–I acceptance contract and historical Linux/physical/Amiga
qualification gates are not redefined by a successful SO test.

The V1/V2-only independent baseline is
`386d97969996864a939bd644c500cd6394a348a3`. The module now includes one portable
engine, reused by V1/V2, the additive V3 API and generic Boot V1. The separate
legacy EFI reader is no longer linked into OFS.so. Boot production sources
remain unchanged at released commit
`dbc6a710dbe88b78318c8b9afbf47bb64e8706b3` (v0.1.38).

| Concern | Implementation and reproducible evidence |
| --- | --- |
| Source/ABI | Original V1/V2 layouts preserved and original assertions retained; V3 and Boot use the same engine. Exact Boot header comparison, export/manifest/no-import/relative-relocation inspection in `standalone/build.py` and `tests/check-boot-so.sh`. |
| Seven geometries/four variants | `tests/ofs_v3_test.py`: 56 two-way producer/consumer round trips (seven sizes × DOS0/2/4/6 × own/foreign producer), independent LNFS reader comparisons and remounts. Oracle limitations and the narrowly corrected DOS4 cache-type defect are in the provenance document. |
| Native OFS file data | Header/owner/sequence/checksum and forward-chain/reversed-pointer agreement; extension rollover and shrink across multiple lists; random and EOF reads, zero-filled growth, overwrite and 32-bit length overflow checks in `tests/ofs_complete_test.py`. |
| Namespace/metadata | Every variant/size exercises create, mkdir, rename/move/replace, unlink, file and directory hard-link promotion, symbolic targets/loops, protection, uid/gid, comments, exact DateStamps and labels. DOS0 and international high-byte case behavior are distinguished. |
| Large directories | 600 entries with collisions, enumeration, rename and deletion for DOS0/4/6. The sanitizer run uses 128 entries per profile to retain cache-chain coverage while bounding instrumentation cost; ordinary portable and native qualification uses 600. |
| DOS4 | Fresh root/user-directory cache creation, authoritative metadata comparison, multiple cache pages, shrink/free, file-size/comment/ownership/date updates and rejection of malformed owners/counts/cycles. Read-only Boot enumerates authoritative hashes. |
| DOS6 | Inline/overflow comment ownership/checksums, allocation cleanup, rename, full-volume hard-link promotion by recycling the removed alias, existing comment-block reuse, root DOSType echo and used-block accounting/recovery. Writers cap names at 106 Latin-1 bytes; the reader also accepts a tested 107-byte raw fixture. |
| Allocation | Writable mounts reject contradictory allocation, even if the bitmap-valid flag is set. ALLOW_DIRTY sessions block ordinary mutations until successful explicit recovery. Full/near-full images return NO_SPACE before ordinary file growth/create writes; partial bitmap pages, multiple bitmap extensions, duplicate/cyclic keys, configurable reserved counts and odd/even roots are tested. |
| High addresses/maximum file length | Virtual read-only fixtures run the actual SO at unsigned keys above 2^31 at every B, with byte offsets above 1 TiB. The 32768-byte fixture reads the end of a UINT32_MAX-byte file through multiple extension lists. These are arithmetic/address tests, not fully populated multi-terabyte media or physical-write evidence. |
| Failure/recovery | Every write and flush barrier of actual overwrite, growth, mkdir, cross-directory rename and unlink is failed in turn (DOS0/4/6). Successful commands use the dirty/valid bitmap protocol. Explicit recovery rebuilds allocation/counters only after a consistent ownership scan; structural contradictions remain errors. Corrupt data/pointers/checksums/cache/comments and repeated failed bitmap indexing are tested. |
| Memory/arithmetic | `tests/check-sanitizers.sh` instruments the actual engine with AddressSanitizer and UBSan. Release SO inspection separately requires zero runtime imports. |
| Boot interface | The actual released generic ELF loader and unchanged UEFI bridge tests consume OFS.so across all 28 variant/size combinations, including UTF-8 names, seek/EOF, whole-disk rejection, read-only operations, trust/memory protection and 26 malformed ELF cases. Portable/native objects follow the same path. |
| Actual firmware handoff | `tests/test_boot_linux.py` independently creates an OFS volume containing a real Linux kernel/initramfs. Released Boot has only OFS.so staged; Linux prints the success marker. CI requires portable/native DOS0 and a 32768-byte DOS6 handoff plus a broken-module/no-fallback rejection. Image hashes must remain unchanged. |
| Packaging/publication | Portable DEB/direct SO and source-embedded native RUN; native build uses measured PGO/LTO. Extracted package payload/BUILD-INFO/hash/API behavior are verified. Main publication depends on filesystem, Boot and preserved transitional EFI-build jobs and creates an immutable version tag with checksums. |

## API and limits

The public API is `native/filesystems/ofs/module/ofs_v3.h`. Byte callbacks must
be exact, partition-relative and bounded to `volume_bytes`. A successful flush
must mean durable host storage completion. Writable mounts require allocation
callbacks and a successful full ownership/bitmap scan before enabling writes; Boot's read-only streaming path has bounded internal memory and no
host heap. V3 can take explicit filesystem B/reserved/root options; its normal
reserved default is two. Device-sector size is not filesystem block size.
Space reports allocated/free filesystem-block capacity, including the native
OFS data-header overhead in any eventual payload allocation.

Dates are native 1978-epoch local DateStamps with 50-Hz ticks. The library does
not infer a timezone. Protection bits remain raw Amiga flags with independent
read/write/delete inhibition. File and user-directory hard-link creation is supported, including cyclic
aliases, writes through aliases and deletion/promotion. Promotion chooses a
rooted alias even when a newer alias is inside the original tree; an unlink
which would orphan the only rooted directory tree is refused before writes.
The special volume root is not a user-directory link target. Symbolic targets preserve native Amiga colon
and repeated-slash semantics. Cross-volume symbolic targets are not resolved
without a host volume resolver.

The disk format is not journaled. A failed operation may leave a dirty bitmap,
a partially updated namespace or data, and requires inspection. The module
does not claim power-loss atomic rename or rollback, infer lost intent, or
silently repair contradictory metadata. A terminal flush failure is still an
error even if the in-memory image happens to contain complete final bytes.

## Platform gates still open

The existing Linux VFS adapter still has its historical page-size/address and
execution qualification work. It is not converted to the shared engine by
this release. The standalone GNU-EFI adapter is preserved/build-tested, but
Boot uses its generic SO bridge. Windows is outside this SO/Boot release.
Original Amiga hardware/handler interoperability, arbitrary RDB/physical-media
and cross-OS original-handler qualification remain open. The existing
Defragmenter OFS native engine is not migrated to consume this SO here.

These are explicit scope boundaries, not evidence that the requested shared
module or its real Boot handoff is absent. Do not turn this release into an
unqualified claim that every platform row of the original DOS0 contract has
closed. No physical disk, ESP or NVRAM changes are part of these tests.

## 0.1.1 directory-link completion

`tests/ofs_complete_test.py` covers directory hard-link records, alias-to-alias
creation, mutations, cyclic traversal, rooted promotion, remount, DOS4 cache
consistency and full-volume DOS6 comment-block recycling at every geometry
and profile. Each write/flush of directory-link creation and promotion is
also failed in turn. `tests/check-boot-so.sh` reads the same file through
both its independently produced original directory and its V3-created alias
using the unchanged released Boot ELF loader at all 28 variant/geometries.

OFS publication is admitted by Infiltrator-Repository's canonical catalogue.
InfiltratorOS 1.0.43 adds a verified native OFS job through the 62-component
snapshot; the previous 61-component endpoint is retained for old installers.
Publication and provisioning remain distinct from physical ESP deployment.

The same metadata tests exercise empty root labels in formatting/relabeling
and shared comments through both file and directory aliases. A metadata setter
and every alias reader must address the same original object, including LNFS
overflow comments; an alias retains its own name and namespace membership.
