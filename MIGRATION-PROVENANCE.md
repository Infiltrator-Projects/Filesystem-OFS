# Source and test provenance

The independent repository starts at `386d97969996864a939bd644c500cd6394a348a3`.
Its OFS sources were imported from Infiltrator-Projects/Filesystems commit
`7473534b1afbd1e9b14f8ab9ae0769fc6be2126e`. Existing copyright and SPDX notices
are preserved. New portable allocation, namespace, data and checking code
adapts those OFS algorithms and the canonical OFS specification, rather than
using FFS raw-data semantics. The SO exports the original unchanged V1/V2
layouts and additive V3 and generic Boot V1 interfaces.

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
Boot tests from released Boot commit
`dbc6a710dbe88b78318c8b9afbf47bb64e8706b3`. The loader and UEFI bridge sources
are compiled directly from that unchanged checkout. The module ABI header is
byte-identical to its `include/ifs_boot_module.h`. The independent OFS fixture
replaces the FAT-specific media producer; firmware, kernel, initramfs and
trust/ELF rejection assertions remain real tests of Boot's consumer.

This evidence demonstrates disposable-image software compatibility. It does
not establish interoperability with every original Amiga handler or certify
physical media, a Windows driver, or the transitional Linux VFS adapter.
