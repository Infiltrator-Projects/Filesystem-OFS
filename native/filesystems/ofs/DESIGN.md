# Amiga OFS canonical implementation

**Normative disk format and behavioral contract:** [OFS_SPECIFICATION.md](OFS_SPECIFICATION.md).

**DOS0 feature-completion qualification:** [DOS0_COMPLETION.md](DOS0_COMPLETION.md). **Evidence and implementation status:** [DOS0 completion matrix](../../../docs/OFS_DOS0_COMPLETION_MATRIX.md). This is the evidence checklist for claiming original OFS functionality complete; it is not a completion claim. The source must be evaluated against that external-source-backed specification; implementation details do not redefine authentic DOS0/DOS2/DOS4/DOS6 on-disk rules.

OFS in this repository is the canonical Infiltrator Projects OFS implementation. The source tree is authoritative.

There must be one OFS engine only. No FUSE, vendored, reference, compatibility, or alternate implementation belongs in the active source tree or application catalogue.

OFS is separate from FFS and SFS. Shared AmigaDOS format concepts do not imply a shared filesystem engine.

Qualification requires supported-target builds, destructive read/write verification, independently created test media, and source review confirming that the active implementation is project-authored code.

## Authoritative shared engine

`core/ofs_{volume,nodes,files,namespace,check,strings,cache}.c` implements the
portable read/write engine. Preserved V1/V2, full V3 and Boot V1 adapters all
use that engine. OFS.so has no libc imports, constructors or platform I/O;
the host supplies bounded partition-relative byte I/O, heap and persistence.

V3 implements DOS0/2/4/6 at all seven block sizes, unsigned keys, formatting,
file/directory operations, metadata, file and directory hard links, native
symbolic targets, ownership verification and explicit bitmap recovery.
Directory links include cyclic aliases and deletion/promotion with a preserved
rooted parent route. V1/V2 retain their historical binary contract.

The Linux VFS and standalone EFI adapters remain transitional separate
consumers. Their platform qualification and the original DOS0 A–I checklist
are retained; feature implementation in the shared engine does not establish
physical-disk, original-handler or kernel-root deployment evidence.
See [shared-module qualification](../../../docs/SO_COMPLETION.md) for the
reproducible current tests and their limits.
