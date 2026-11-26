# Filesystem-OFS

## 1. Purpose and format scope

Amiga Original File System (DOS0, DOS2, DOS4 and DOS6), distinct from FFS. This repository owns the format-specific implementation of `OFS.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

OFS retains its 24-byte data-block header and checksum. The native geometry includes block sizes from 512 to 32768 bytes, 32-bit block keys and 64-bit volume offsets. File headers use reversed pointer tables and extension chains; directory hashes, bitmap allocation, native links and cache records must remain consistent. DOS6 supports long names and overflow comments; UTF-8 API capacities are measured in bytes, not on-disk Latin-1 characters.

## 3. Common V2 implementation and capabilities

Common V2 declares probe, mount, stat, enumeration, read, write, create, remove, rename, check and format; the native engine also owns file growth, truncate, links, metadata and volume operations. Dedicated relocation/recovery provides Analyse/map, staged Defrag, 10% Growth reserve, stop and bound-target recovery. The freestanding read profile supports streaming and seek. Consult exact masks for each operation. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Directory links can form cycles, so traversal must be bounded; refuse removal of the only rooted name of a cyclic directory tree. DOS4 cache and DOS6 block-count/comment changes must accompany namespace mutation. Whole-volume structural validation precedes explicit bitmap recovery. Independent Amiga media comparison does not establish physical BOOT handoff or every crash scenario. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/OFS.so` and `/System/Resources/Documentation/Filesystem-OFS.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-OFS/actions/runs/45526928256) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-OFS/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](native/filesystems/ofs/module/ofs_common.c)
- [Native design or source provenance](native/filesystems/ofs/OFS_SPECIFICATION.md)
- [Migration and source history](MIGRATION-PROVENANCE.md)
- [Plain-text PDF drawing source (not Apple Pages)](docs/OFS.pdfstreams) and [PDF](docs/OFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)
- [Native completion matrix](docs/OFS_DOS0_COMPLETION_MATRIX.md)
- [Shared-object qualification](docs/SO_COMPLETION.md)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

The canonical specification and installed manual include the historical release chronology, DOS0-DOS7 distinctions, RDB/Kickstart boot history, validation and repair tools, TD64/NSD and large-media boundaries, directory-cache limitations, LNFS and independent source references. Historical handler support is not a claim of current provider parity.

The [engineering reference](docs/OFS-TECHNICAL-REFERENCE.md) incorporates the detailed format specification and worked exercises. The previous illustrated manual is preserved in the PDF.
