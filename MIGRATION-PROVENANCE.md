# Source and test provenance

The independent repository starts at `386d97969996864a939bd644c500cd6394a348a3`.
Its OFS sources were imported from Infiltrator-Projects/Filesystems commit
`7473534b1afbd1e9b14f8ab9ae0769fc6be2126e`. Existing copyright and SPDX notices
are preserved. New portable allocation, namespace, data and checking code
adapts those OFS algorithms and the canonical OFS specification, rather than
using FFS raw-data semantics. The SO exports the original native layouts, the later read/write extension and the
firmware adapter used at that checkpoint.

Defrag V2 is an additive process-hosted interface in the same freestanding SO.
Its OFS relocation uses the canonical engine's verified namespace, data cursors
and allocation maps. Defragmenter supplies filesystem-neutral services only.
Its Test Media producer calls this SO's V3 methods; retained AFFS parsing there
is an independent disposable-image verifier and legacy-journal compatibility.
The generic Linux VFS compatibility header is preserved from Filesystem-FAT16
commit `4ce1c01130845f9e065ed0faad5766c4d9a99ee2`; it contains only kernel API
version adaptation, and each native KO compiles its own copy.

The native OFS KO now compiles that same portable engine through separate
translation units and a Linux VFS binding. The old AFFS-derived Linux storage,
namespace and lifecycle implementation has been removed. Device-buffer I/O is
independent of filesystem block and host page sizes. Deferred open-inode
deletion is shared engine code with its own every-barrier failure tests.

The normative reference remains `native/filesystems/ofs/OFS_SPECIFICATION.md`.
The external format references include the ADFlib author's
[Amiga disk FAQ](https://adflib.github.io/FAQ/adf_info.html) and Olaf Barthel's
[DCFS/LNFS structures](https://wiki.amigaos.net/wiki/DCFS_and_LNFS_Low_Level_Data_Structures).
This repository is licensed GPL-3.0-or-later; `LICENSE` contains GPL version 3.
Historical source notices remain authoritative for the files carrying them.

The independent interoperability test dependency is amitools **0.8.1**, source
archive `amitools-0.8.1.tar.gz`, SHA256
`f622c0725c15737e7d4820ed147f930f4cfb7b80b04c786c2bc3943b799faf7e`.
It is downloaded only for qualification and is not linked or packaged into
OFS.so. The adapter supplies regular-file block I/O for all seven OFS sizes.

The pinned oracle has known limitations, recorded rather than hidden:

- Its DOS4 producer always supplies/writes a zero cache type byte. The test
  device corrects precisely that byte from the producer's already-written
  entry header, then recomputes the cache checksum. DOS0/2 tests use the
  unmodified producer; DOS4 comparisons are qualified by this correction.
- Its high-level directory parser does not support hard or symbolic links.
  SO link operations are checked with direct byte/ownership tests and the
  SO verifier; symbolic links are removed before the independent high-level
  parser opens a round-trip fixture. This is not external link qualification.
- Its LNFS inline-comment writer copies the name as the comment. Independent
  LNFS producer cases use short names; SO-written long names and comments are
  checked by the oracle reader and raw specification assertions separately.
- Its root calculation ignores reserved-block-aware odd-volume placement.
  Independent comparisons use even volumes; odd and configurable reserved
  geometry have distinct raw structure tests.

`tests/ofs_boot_loader_test.c` and `tests/test_boot_linux.py` adapt the GPL
Boot tests. The Common V2 tests compile the loader and UEFI bridge directly
from released Boot 0.1.45, commit `93aed1b866f502ac61f6f23a20c524c7fad62812`.
The Common V2 headers are byte-identical. An independent OFS producer
supplies the images used for ELF rejection, volume labels, directory
enumeration and kernel/initramfs handoff.

This evidence demonstrates disposable-image software compatibility. It does
not establish interoperability with every original Amiga handler or certify
physical media, a Windows driver, or Linux kernel ABIs beyond the pinned VM.
