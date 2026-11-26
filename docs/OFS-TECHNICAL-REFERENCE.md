# OFS engineering reference

The following source specification is retained for original-format detail. Its dated project implementation requirements are historical, not current ABI qualification.

# AmigaDOS Original File System (OFS) — Canonical Format Specification

**Revision:** 1.0 (24 October 2026)  
**Location and ownership:** `native/filesystems/ofs/OFS_SPECIFICATION.md`  
**Purpose:** Define the OFS **on-disk format, algorithms, semantics and compatibility requirements** independently of any current implementation. This document is the source-level conformance target for the formatter, Linux `ofs.ko`, Windows drivers, the UEFI `OFS_x64.efi` reader and test fixtures.  
**Profiles:** original OFS `DOS\0`; international OFS `DOS\2`; directory-cache OFS `DOS\4`; and long-name OFS `DOS\6` (LNFS). **All four are OFS**, not FFS. LNFS uses distinct on-disk entry and root metadata defined in §13A.  
**Certification status:** The format is externally documented; **the project implementation is not yet fully certified against this specification**. Track implementation status elsewhere.

## 1. Source authority and interpretation

Original format descriptions and cross-checks:

1. Laurent Clévy, **The .ADF (Amiga Disk File) Format FAQ**, v1.14, ``4.1–4.7, 5.1–5.4, 6: https://adflib.github.io/FAQ/adf_info.html — comprehensive independent block tables, algorithms, examples and Amiga hard-disk layout.
2. **Amiga ROM Kernel Reference Manual: AmigaDOS**, filesystem structures and variants: https://developer.amigaos3.net/sites/default/files/downloads/2024-10/Amiga_ROM_Kernel_Reference_Manual_DOS.pdf
3. **Linux AFFS documentation** (specification/driver-limit distinction and official DOS variants): https://www.kernel.org/doc/html/v6.7/filesystems/affs.html
4. Linux AFFS and ADFlib source are useful **secondary interoperability witnesses**, not licenses to substitute FFS storage mechanics for OFS.

**[FORMAT]** below denotes original on-media structure/rule. **[VARIANT]** denotes a supported OFS subformat. **[POLICY]** denotes additional project safety, validation or qualification requirements, not an assertion about historical AmigaDOS. **[OPEN]** indicates an aspect for which historical implementations or sources differ and where independent test images are still necessary.

**MUST** and **MUST NOT** govern a conforming implementation of the selected profile. A reader MAY support a declared read-only subset but MUST reject unsupported input without silently misidentifying it. A **writer** must preserve every invariant required for the profile it creates. When code and this contract disagree, resolve the question using original sources and independent fixtures; never redefine the format to suit the code. An implementation's current limitations are **not** format limits.

## 2. Canonical OFS identifiers

A filesystem volume begins with the four-byte DOSType; bytes 0..2 are ASCII `D`, `O`, `S` and byte 3 is a binary version byte, **not** the printable character '0'.

| DOSType (BE32) | OFS format | Name and on-disk extension |
| --- | --- | --- |
| `0x444F5300` / `DOS\0` | Original OFS | Original 30-byte names and OFS data blocks |
| `0x444F5302` / `DOS\2` | International OFS | International Latin-1 folding; classic metadata layout |
| `0x444F5304` / `DOS\4` | Directory-cache OFS | International folding, classic metadata, additional directory-cache blocks |
| `0x444F5306` / `DOS\6` | Long-name OFS (LNFS) | International folding; new 112-byte NaC name/comment field, comment overflow blocks and root accounting fields |

All **four** are bona fide OFS on-disk profiles. No FFS identifiers belong in the OFS profile table; those live in [FFS_SPECIFICATION.md](../ffs/FFS_SPECIFICATION.md). A DOS-type value not listed above must not be silently mounted as one of these four formats.

**`DOS\4`** always uses international name handling and directory-cache metadata. **`DOS\6`** uses international name handling **without** a directory cache (LNFS and DCFS are separate alternatives, not stackable flags). All OFS versions retain OFS's 24-byte regular-file data-block header and `B-24` byte payload. The on-disk name/comment and root metadata must be decoded according to the variant; the classic 30-byte entry-name layout is **not valid** for `DOS\6`. Source: Olaf Barthel, [DCFS and LNFS Low Level Data Structures](https://wiki.amigaos.net/wiki/DCFS_and_LNFS_Low_Level_Data_Structures).

**[VARIANT]** muFS types are distinct later compatibility modes with additional ownership semantics and their own DOSTypes. They should not be accepted by applying the plain DOS0/2/4/6 profile blindly.

### Formatting profile selection — a required writer interface

**[POLICY]** The OFS formatter MUST accept an explicit **--variant dos0|dos2|dos4|dos6** choice for new volumes, with **dos0 as the default** when no variant is provided. This is a *format profile*, not a software release/version string, and must be independent of block size, label, and whether a target is a disposable image or an explicitly confirmed physical partition.

- **dos0** must write DOS\0, classic OFS 24-byte data headers and ASCII-style name hashing.
- **dos2** must write DOS\2, the same OFS block layout and international Latin-1 name rules for all subsequent create/lookup/rename operations.
- **dos4** must write DOS\4, the same OFS data format and international names **and** an initial valid T_DIRCACHE=33 block for the root (even if there are zero entries), linked through the root's directory-cache pointer and marked allocated in the bitmap. Every later writable directory mutation must keep the cache consistent.
- **dos6** must write DOS\6 with **LNFS root type confirmation and used-block counter**, the international name hash and the distinct NaC/comment metadata shape detailed in §13A. A new empty root still has a 30-byte volume label and does **not** acquire a DOS4 directory-cache block. Any subsequent writable entry creation, rename, lookup, enumeration, unlink or comment update must use the LNFS metadata fields; a writer that only knows the classic fields MUST decline writable DOS6 mounts.

Selecting dos4 **or dos6** must **never** be implemented by changing the four-byte identifier alone. Creation must fail closed if required cache metadata cannot be created and verified. A reader/verifier must discover and validate the actual stored variant, not silently assume the formatter's default. A tool must reject unknown variants, avoid converting existing volumes implicitly, and preserve all physical-target confirmation, geometry and capacity safety gates. Variant selection alone must not waive an unqualified physical disk or boot configuration limit.

## 3. Terminology, encoding, units and geometry

| Symbol | Definition |
| --- | --- |
| `B` | Logical OFS filesystem block size, in bytes |
| `W` | `B/4`, longwords per logical filesystem block |
| `N` | Total number of logical filesystem blocks, keys `0..N-1` |
| `R` | Number of reserved initial OFS blocks; ordinary floppy/default value **2** |
| `K` | Partition-relative filesystem block key, stored as BE32 |
| `H` | Hash/data-block-pointer table capacity per header: `W-56` |
| `P` | Usable payload per OFS data block: `B-24` |
| `Q` | Bits per allocation bitmap block: `8B-32` |

**[FORMAT]** All numeric multibyte disk fields are **big-endian**. Standard longword is four bytes and word is two bytes. A negative block secondary type uses a signed, two's-complement 32-bit field. On-disk pointers (keys) are *zero-based, partition-relative* unsigned 32-bit values, and **zero denotes a null pointer**. The host's 512-byte logical sectors, UEFI Block I/O LBAs, Linux pages, DMA buffers and OFS logical filesystem blocks are not interchangeable.

**[FORMAT]** The recognized OFS family permits filesystem block sizes **512, 1024, 2048, 4096, 8192, 16384 and 32768 bytes**. In this project these seven are the block-size **format-conformance targets**. A formatter must honor the logical block size when writing root/hash/bitmap/tail structures, checksums and data payloads; it must never just write a 512-byte block padded to `B`. Determine `B` from an authoritative partition environment where available or by independently verifying candidate geometries; the classic `DOS` ID alone **does not encode `B`**.

`w[i]` denotes the BE32 stored at byte `4*i` from the **start** of the current block. Expressions of the form `B-200`, `B-80` and `B-4` are **byte offsets relative to that block's end**, and move with `B`. At `B=512`, `H=72`, `P=488` and `Q=4064`. At `B=32768`, `H=8136`, `P=32744` and `Q=262112`.

**[FORMAT]** `K * B` is the partition-relative byte offset of a filesystem block. The storage adapter adds the actual partition start in checked, wide arithmetic. Amiga RDB filesystem partition entries, GPT/MBR sectors, FSHD/LSEG and physical device geometry are **outside** the OFS volume proper. A plain hardfile may contain only the filesystem partition's bytes.

**[LIMIT]** A 32-bit key and 32-bit file-length field imply *arithmetic* limits: fewer than `2^32` addressed logical blocks (subject to reserved keys) and a maximal individual file length of `2^32-1` bytes. `(2^32-1)*B` is an addressing bound (just below **2 TiB at B=512**, just below **128 TiB at B=32768**), **not** a verified filesystem capacity, firmware transfer limit, classical device-driver guarantee or Linux boot qualification. No universal signed 31-bit limitation follows from the on-disk key type. A 32-bit file size does **not** turn into 128 TiB just because `B` grows.

## 4. Boot area and root placement

The first `R` reserved logical blocks comprise the classic boot area; normally `R=2`. The default classic 512-byte boot area is therefore **1024 bytes**, not one generic "boot sector".

| Boot-area byte offset | Size | Meaning |
| --- | --- | --- |
| `0` | 4 | Binary DOSType identifier |
| `4` | 4 | **Bootblock** checksum, special algorithm |
| `8` | 4 | Boot/root hint used by Amiga boot code; an unbootable disk need not rely on it |
| `12..(boot_area_bytes-1)` | remainder | Optional executable Amiga boot code or unused bytes |

The **boot checksum** differs from the metadata checksum. Zero bytes 4..7, sum all BE32 words of the configured bootblock length with one's-complement **end-around carry**, and store one's complement of the final sum at bytes 4..7. An empty/unbootable boot area can legitimately have a DOSType and no usable boot code; mounting OFS is separate from classic AmigaOS bootability. Do not require an arbitrary executable bootblock or invent bootcode during formatting.

The canonical root block key is:

`rootKey = floor((R + N - 1) / 2)`, with `R=2` normally.

Examples: `N=1760 -> 880`, `N=2048 -> 1024`, and **`N=2049 -> 1025`**. `floor(N/2)` is therefore **wrong for an odd count** with `R=2`. Compute this using an intermediate wider than 32 bits and check all bounds. Alternative Amiga DOS environment fields and custom reserved geometry must be interpreted explicitly, rather than inferred from a random first-sector number.

**[POLICY]** A strict writer puts the root at this canonical position. A recovery reader MAY expose a bounded diagnostic search for nonconforming misplaced root blocks; it must not silently treat such output as a standards-conforming formatted volume.

## 5. Common metadata, checksums and time values

**[FORMAT]** Root, directory, file header, file pointer extension, OFS data block and directory-cache block contain a checksum over the **entire `B`-byte logical block**. With the relevant checksum longword zeroed, compute the sum of all `W` BE32 words modulo `2^32` and write `(-sum) mod 2^32` at the checksum offset. A valid block has a modulo-`2^32` sum of **zero** including its checksum.

| Block | Checksum BE32 byte offset |
| --- | --- |
| Root, directory, file header, file extension, OFS file data, cache block | `20` |
| Bitmap block | `0` |
| Boot area | `4`, **different end-around-carry algorithm** |

All metadata parsers must distinguish these two algorithms, validate the block's identity/ownership/position, bound every pointer and reject cycles. A numerically valid checksum **alone does not prove** a block is a correct member of the requested directory or file. New writers should initialize all unused fields to zero, except for explicitly defined flag/mask values.

The classic `DateStamp` is **three BE32 fields**: days since **1 January 1978**, minutes since local midnight (normally `0..1439`), and ticks in **1/50 seconds** (normally `0..2999`). DateStamp uses no stored timezone. The root has separate **root change**, **volume change** and **volume creation** timestamps; these must not be accidentally collapsed into one.

## 6. Root block: canonical byte layout

This table is the classic root. In **DOS6**, two reserved positions acquire mandated values: the used-block counter at `B-44` and root DOSType at `B-16` (§13A.2). All other root geometry remains OFS.

All offset expressions are relative to a single `B`-byte filesystem block. `H = B/4 - 56`.

| Offset | Length | Field / invariant |
| --- | --- | --- |
| `0` | 4 | Primary type `T_HEADER = 2` |
| `4` | 4 | Root header key = 0 |
| `8` | 4 | Sequence = 0 |
| `12` | 4 | Hash table size = `H` |
| `16` | 4 | First data = 0 |
| `20` | 4 | Metadata checksum |
| `24..B-201` | `4H` | Array `hash[H]` of BE32 child keys; zero = empty bucket |
| `B-200` | 4 | Bitmap validity flag; `0xFFFFFFFF` denotes valid |
| `B-196..B-97` | 100 | 25 BE32 bitmap-page keys |
| `B-96` | 4 | First bitmap extension key, or 0 |
| `B-92` | 12 | Root-change DateStamp |
| `B-80` | 1 | Volume name length, `0..30` |
| `B-79..B-50` | 30 | Volume name bytes, padded; Latin-1-compatible single-byte naming |
| `B-49` | 1 | Reserved = 0 |
| `B-48` | 8 | Reserved = 0 |
| `B-40` | 12 | Whole-volume-change DateStamp |
| `B-28` | 12 | Volume-creation DateStamp |
| `B-16` | 4 | Reserved root hash-chain key = 0 |
| `B-12` | 4 | Reserved root parent key = 0 |
| `B-8` | 4 | First `DOS\4` directory-cache key, or 0 |
| `B-4` | 4 | Secondary type `ST_ROOT = +1` |

For `B=512` the bitmap flag is at `0x138`, its first bitmap pointer is at `0x13C`, bitmap extension at `0x1A0`, volume-name length at `0x1B0`, and secondary type at `0x1FC`. For larger `B` **none of these end-relative positions is fixed at its 512-byte offset**.

Validate root type, hash size, correct root location, checksum, legal label length, non-overlapping bitmap/root pointers, and that each referenced directory entry really belongs under the referenced root.

## 7. Allocation bitmap blocks, extensions and free-space accounting

**[FORMAT]** Every nonreserved filesystem block from `R` to `N-1` is represented by **one bitmap bit**. A **set** bit means **FREE**; a **clear** bit means **ALLOCATED**. The `R` reserved initial boot blocks **do not have allocation bits**. Do not reverse this meaning or accidentally include boot blocks in the bitmap bit numbering.

A bitmap block has:

| Offset | Length | Meaning |
| --- | --- | --- |
| `0` | 4 | Whole-bitmap-block checksum |
| `4..B-1` | `B-4` | `W-1` BE32 bitmap words, 32 bits each |

Each bitmap block covers `Q = (W-1)*32 = 8B-32` volume block keys. For nonreserved block `k`:

`relative = k - R`; `bitmapIndex = floor(relative/Q)`; `within = relative mod Q`; `word = floor(within/32)`; `bit = within mod 32`.

Bitmap data words are big-endian, but the **lowest-numbered covered block corresponds to bit 0 (LSB)** of its 32-bit word. To allocate, clear the relevant bit; to free, set it. Root, bitmap pages, bitmap extensions, directories, file headers, file extensions, data blocks and cache/link records must all be marked allocated. Cross-check a block's bitmap state with its actual use.

First 25 bitmap-page keys live in the root. Additional bitmap-page keys come from the bitmap **extension** linked list:

| Bitmap-extension offset | Meaning |
| --- | --- |
| `0..B-5` | `W-1` BE32 bitmap-page keys |
| `B-4` | Next bitmap-extension key; 0 ends chain |

The bitmap extension is **not an ordinary metadata header**, and its words are **not a directory hash**. The chain must terminate and must not cycle. Number of bitmap pages needed is `ceil((N-R)/Q)`. A conforming writer must not lose track of partial final coverage, uninitialized tail bits, bitmap-page ownership or extension bookkeeping. **[POLICY]** Tail bits beyond the volume end are ignored for free-space accounting and should be marked not-free in newly created media; compatibility readers must not treat out-of-volume bits as allocatable, whatever their stored values.

A valid bitmap checksum means its bit contents are internally checksum-consistent, **not** necessarily in agreement with real allocated metadata or data; whole-volume verification has to perform that separate cross-check. The bitmap-valid root flag and its update protocol matter for crash recovery; a full writer must manage it and durable writes coherently rather than treating the flag as an aesthetic field.

## 8. Directory header, hash tables, name rules and lookup

The tail positions and 30-byte name field in this section apply to **DOS0/2/4** only. DOS6 uses the replacement nonroot NaC layout in §13A.

**[FORMAT]** A regular directory block is `T_HEADER=2` with secondary `ST_USERDIR=+2`. Its head has the same 24-byte common prefix as a file header: own key at `4`, checksum at `20`, and a table of `H` BE32 **hash-bucket** keys at `24..B-201`. **Unlike a root block**, its three BE32 fields at offsets `8`, `12` and `16` are reserved and **must all be zero**; in particular, offset `12` is **not** the hash-table size. A verifier that demands `H` at offset `12` in every user directory rejects valid original AmigaDOS media. This distinction is specified by the original AmigaOS UserDirectoryBlock structure and ADFlib's DOS0 layout. The standard tail is:

| Offset | Size | Directory tail |
| --- | --- | --- |
| `B-200` | 4 | Reserved |
| `B-196` | 2 | UID (BE16) |
| `B-194` | 2 | GID (BE16) |
| `B-192` | 4 | Protection bits |
| `B-188` | 4 | Directory file-size field unused |
| `B-184` | 1 | Comment length `0..79` |
| `B-183..B-105` | 79 | Comment bytes |
| `B-104..B-93` | 12 | Reserved |
| `B-92..B-81` | 12 | Change DateStamp |
| `B-80` | 1 | Name length `1..30` |
| `B-79..B-50` | 30 | Name bytes |
| `B-49..B-41` | 9 | Reserved |
| `B-40` | 4 | First hard-link key where supported |
| `B-36..B-17` | 20 | Reserved |
| `B-16` | 4 | **Hash chain**: next entry with same bucket |
| `B-12` | 4 | Parent directory/root key |
| `B-8` | 4 | `DOS\4` first directory-cache key, otherwise zero |
| `B-4` | 4 | Secondary type `ST_USERDIR=2` |

The root and user directory **share the name-hash algorithm**; their **tails differ**, notably in bitmap versus file metadata.

**[FORMAT]** Names are length-prefixed **single-byte** strings, usually Amiga Latin-1. For DOS0/2/4, maximum **30 bytes** per entry; for DOS6 use the NaC layout of §13A and a 106-byte interoperable writer limit. A name is *not* an arbitrary UTF-8 sequence. `:` and `/` are not legal literal filename/volume-name characters; a reader must validate length before reading or folding and not assume a NUL terminator. International OFS is still the same 30-byte structural profile, not a long-filename extension.

Directory hash:

1. Begin `h = nameLength`.
2. For each byte `c` in the name, `h = (13*h + fold(c)) & 0x7FF`.
3. Select `bucket = h mod H`.
4. The directory/root hash array contains the first child block key in that bucket, or zero.
5. Follow the child's **hash chain** field at `B-16` until a folded-name match is found or a null key ends the chain. Each entry must have this directory as its parent; its own computed bucket must equal the chain bucket. Reject cycles, out-of-range references, duplicate-folded names in the same directory and checksum/type mismatches.

**Original OFS `DOS\0` folding:** ASCII lowercase `a..z` to uppercase `A..Z`. High Latin-1 bytes retain original case semantics as required by the selected original character table.

**International `DOS\2` and `DOS\4` folding:** ASCII `a..z` additionally Latin-1 lowercase `0xE0..0xFE` is mapped by subtracting `0x20` **except `0xF7`** (division sign). Do not use locale-dependent C `toupper()` on host Unicode input or set `DOS\4` to ASCII-only merely because its ID's international bit is not set.

**[POLICY]** Directory enumeration must cap work for a damaged hash chain without globally inventing a limit on **valid** directory size: apply bounded traversal/visited-key cycle detection and a controlled error. A 512-entry limit is a particular EFI-reader policy, **not** an OFS filesystem format limit. Search and readdir must agree on the same bucket representation and hash case rules.

## 9. Regular file header and extension blocks

The traditional file-header tail/name/date fields here apply to **DOS0/2/4**. DOS6 replaces them with the NaC and long-name timestamp/comment metadata (§13A); file data block sequence and extension-pointer storage remain OFS.

A file begins with a header block `T_HEADER=2` and `ST_FILE=-3`. It is a metadata object, not the payload itself.

| Offset | Size | Regular-file header |
| --- | --- | --- |
| `0` | 4 | `T_HEADER=2` |
| `4` | 4 | Own header key |
| `8` | 4 | `highSeq` = number of data-block pointers in this header |
| `12` | 4 | Reserved |
| `16` | 4 | **firstData** key, first OFS data block or 0 |
| `20` | 4 | Metadata checksum |
| `24..B-201` | `4H` | `H` BE32 data-block keys |
| `B-200` | 4 | Reserved |
| `B-196` | 2 | UID |
| `B-194` | 2 | GID |
| `B-192` | 4 | Protection bits |
| `B-188` | 4 | **File length in bytes**, BE32 |
| `B-184` | 1 | Comment length `0..79` |
| `B-183..B-105` | 79 | Comment bytes |
| `B-104..B-93` | 12 | Reserved |
| `B-92..B-81` | 12 | Last change DateStamp |
| `B-80` | 1 | File name length `1..30` |
| `B-79..B-50` | 30 | File name |
| `B-49..B-45` | 5 | Reserved |
| `B-44` | 4 | Real-entry pointer / reserved except supported hardlink context |
| `B-40` | 4 | First hard-link key where supported |
| `B-36..B-17` | 20 | Reserved |
| `B-16` | 4 | Next entry in containing directory hash chain |
| `B-12` | 4 | Parent directory/root key |
| `B-8` | 4 | First file **extension** key or zero |
| `B-4` | 4 | `ST_FILE=-3` |

**[FORMAT]** The data-block key table is interpreted **in reverse order**: the first file data block is in the **last** table entry (`table[H-1]`), the second at `table[H-2]`, etc. For `B=512`, the first is `table[71]`. `highSeq` must be in `0..H`. Do not assume `table[0]` is the first logical data block, and do not treat spare entries as valid data.

When a file uses more than `H` data blocks, file-extension blocks (`T_LIST=16`) chain from header byte `B-8`. Their structure is:

| Offset | Size | File pointer extension |
| --- | --- | --- |
| `0` | 4 | `T_LIST=16` |
| `4` | 4 | Own extension key |
| `8` | 4 | `highSeq`: valid data pointers in this extension |
| `12` | 4 | Reserved, written as zero |
| `16` | 4 | Reserved in the Amiga ROM Kernel Reference Manual. Some implementations store a copy of the first data key in this table |
| `20` | 4 | Metadata checksum |
| `24..B-201` | `4H` | BE32 data block keys, **last valid table entries first** |
| `B-200..B-17` | 184 | Unused/reserved metadata area |
| `B-16` | 4 | Reserved |
| `B-12` | 4 | Parent/owner **file-header** key |
| `B-8` | 4 | Next file extension key, 0 if final |
| `B-4` | 4 | `ST_FILE=-3` |

The [Amiga ROM Kernel Reference Manual, DOS §13.6.8](https://developer.amigaos3.net/sites/default/files/downloads/2024-10/Amiga_ROM_Kernel_Reference_Manual_DOS.pdf) specifies zero at byte 16. The [earlier AmigaDOS Technical Reference Manual, Figure 1-D](https://www.pagetable.com/docs/amigados_tripos/amigados_manual.pdf) labels that word as a first-data key, and Linux AFFS fills it. InfiltratorOS creates extensions with zero there and uses the reversed table for lookup. When relocating an existing extension, it preserves zero or updates the populated copy to the new first data key.

The owning file header and any extensions must agree about referenced data block keys and file size. An empty file has a file header, length zero and **no data blocks**; do not allocate an empty spurious data chain.

## 10. OFS data block — the essential non-FFS layout

Every nonempty regular file contains a sequence of `T_DATA=8` blocks with **exactly 24 bytes of structure** followed by at most `P=B-24` file-data bytes.

| Byte offset | Size | Mandatory content |
| --- | --- | --- |
| `0` | 4 | `T_DATA=8` |
| `4` | 4 | Owning file-header block key |
| `8` | 4 | **1-based** data-block sequence number |
| `12` | 4 | Valid payload byte count `0..P` (normally `P` except final block) |
| `16` | 4 | Next OFS data block key, 0 at end |
| `20` | 4 | Metadata/data-block checksum |
| `24..B-1` | `P` | Data payload; bytes beyond valid count are not part of file |

For a 512-byte OFS block `P=488`. A 17,029,512-byte file requires `ceil(17029512/488)` OFS data blocks, **not** `ceil(17029512/512)`. Every linked block must have sequential sequence number, matching file owner, valid payload size and checksum. The file's stored 32-bit length governs EOF. The first block is reached via the file header's `firstData` and/or its reversed pointer table; all file pointer arrays/extension chains should independently agree with the data linked chain.

**[FORMAT]** OFS can be traversed by its sequential `nextData` links as well as by file metadata/extension pointer tables. FFS does **not** have the former. This is a material functional distinction, not an implementation option to erase. When writing, maintain **both** OFS traversals coherently; a reader must fail safely on cycles, out-of-volume links, wrong owner/sequence, holes inconsistent with the selected profile or contradictory checksums.

**[POLICY]** Invalid media must not cause unchecked unsigned multiplication/subtraction, buffer overread, nonterminating traversal, or speculative block reads beyond partition bounds. Per-file upper length is still `UINT32_MAX` bytes.

## 11. Hard links and symbolic links (version-sensitive)

**[VARIANT]** Historically link support and interoperability differed among filesystem-handler/AmigaDOS revisions and had known compatibility issues. Do not claim that every original DOS\0 floppy handler can write or follow all link types. An implementation that declares **link support** uses the established AmigaDOS metadata records, independently tests them and handles cycles.

Hard links use primary `T_HEADER=2`, secondary `ST_LINKFILE=-4` or `ST_LINKDIR=+4`, and a metadata tail with name, parent, `hashChain`, **realEntry** at `B-44` and **nextLink** at `B-40`. An original object's link-chain head is also held at `B-40`. All link targets must be within the appropriate owning volume. Creating/deleting one link must maintain directory hashes and the original entry's separate link list, and must **not** free a file's shared data while any hard link still needs it. Hard links to directories can form traversal cycles.

Soft links use primary `T_HEADER=2`, secondary `ST_SOFTLINK=+3`, and their symbolic pathname is a **NUL-terminated Amiga path** beginning at byte **24** (not a 32-bit block key). The tail includes softlink's own metadata, name, hash chain and parent. Amiga path syntax (device/volume prefix followed by `:`, repeated `/` for parent semantics) is **not identical** to POSIX path syntax. Translation on a Linux mount is a separate adapter responsibility and must preserve meaningful relative/absolute semantics; unsupported cases must fail rather than write incorrect targets. Resolve softlinks with a bounded traversal depth.

Link support must be listed per DOS profile and validated with authentic fixtures; do not silently treat unknown `ST_*` records as files.

## 12. DOS\4: directory-cache block layout and semantics

**[VARIANT]** `DOS\4` is **international OFS with an additional directory-cache layer**, not FFS. The ordinary root/user directory **hash tables remain valid and authoritative**. Directory-cache records are auxiliary accelerators. Each directory/root tail's `B-8` field points to the first cache block, or 0.

| Cache-block offset | Length | Meaning |
| --- | --- | --- |
| `0` | 4 | Primary type `T_DIRCACHE=33` |
| `4` | 4 | Own cache key |
| `8` | 4 | Owning directory/root key |
| `12` | 4 | Count of variable-length cached records |
| `16` | 4 | Next cache key, 0 ends chain |
| `20` | 4 | Metadata checksum |
| `24..B-1` | `B-24` | Packed, variable-length records |

Each record begins at an even-aligned offset:

| Record-relative offset | Size | Meaning |
| --- | --- | --- |
| `0` | 4 | Referenced entry header/link key |
| `4` | 4 | File length; zero for directories/links |
| `8` | 4 | Protection flags |
| `12` | 2 | UID |
| `14` | 2 | GID |
| `16` | 2 | Date days (short) |
| `18` | 2 | Date minutes (short) |
| `20` | 2 | Date ticks (short) |
| `22` | 1 | Secondary entry type |
| `23` | 1 | Name length, `1..30` |
| `24` | variable | Name bytes |
| `24+nameLength` | 1 | Comment length, `0..22` |
| `25+nameLength` | variable | Short comment bytes |
| after record | 0 or 1 | Alignment padding to even byte boundary |

A directory-cache reader must validate every record's length, alignment, ownership, checksum, cache chain and referenced entry key. **[POLICY]** A read-only driver may ignore caches and enumerate through the authoritative hash tables after validating that the volume is truly OFS; this avoids trusting inconsistent caches. A writer for `DOS\4` **must** maintain cache records whenever it creates, removes, renames or updates directory entries, or else decline writable mounting. Do not write `DOS\4` as though it were `DOS\0` and leave stale caches.

## 13. Names, attributes, ownership and timestamps

- Root labels and normal file/directory names have **length-prefixed** single-byte encoding and **30-byte** classic maximum. The forbidden `:` and `/` delimiters cannot appear as literal names. Validate length before reading; unused array bytes are padding.
- Directory lookup is case-insensitive according to the selected DOS profile. The identifier affects both **hash generation** and **name comparison**; converting UTF-8 to Latin-1, replacement policy and display case belong to host adapters.
- A metadata `protect` BE32 field contains Amiga permission/protection flags (e.g. low bits: delete, execute, write and read **prohibited** when set; archive/script/pure and owner/group/other flags are separate). Do not invert Amiga's low four negative logic into POSIX mode bits or assume that POSIX uid/gid owns the on-disk semantics. Mapping to Linux chmod/chown and EFI FileInfo is adapter-specific and must be round-trip tested.
- File/directory comments are length-prefixed: up to **79 bytes** in the main header. DOS\4` cached comments are shorter and must not be mistaken for authoritative full comments.
- Root timestamp fields distinguish creation, root change and entire-volume change; file/directory DateStamps record changes. Never change creation time when simply renaming a file; document consistency policy for crash/recovery.

## 13A. DOS\6 — canonical OFS Long Name File System (LNFS)

**[FORMAT]** LNFS is an OFS variant added by Olaf Barthel during the later Amiga filesystem redevelopment. Its original 2001 low-level definition is independently described in the official AmigaOS documentation: [DCFS and LNFS Low Level Data Structures — Long Name File System](https://wiki.amigaos.net/wiki/DCFS_and_LNFS_Low_Level_Data_Structures#Long_Name_File_System_.28LNFS.29). It is **not** a directory-cache mode, and it is **not** FFS. Only the metadata **changes below**; OFS data header, data checksum, file sequence and allocation bitmap semantics remain mandatory.

### 13A.1 Name and comment layout — file, directory and links

For **all nonroot directory entries** (regular file header, directory header, soft link, hard link), the classic **separate** comment/date/name fields are replaced. Offsets are from the end of a `B`-byte block:

| Offset | Length | Meaning in DOS6 |
| --- | --- | --- |
| `B-184` (`w[W-46]`) | 112 | `NaC`: name and comment, two back-to-back **BCPL length-prefixed** byte strings |
| `B-72` (`w[W-18]`) | 4 | `CommentBlock`: pointer to an optional overflow-comment block, or zero |
| `B-68..B-61` | 8 | Reserved; must be zero on new media |
| `B-60..B-49` (`w[W-15..W-13]`) | 12 | DateStamp (days, minutes, ticks): **moved** from the classic position |
| `B-48..B-17` | 32 | Existing hard-link fields/reserved positions, same logical meaning as applicable |
| `B-16`, `B-12`, `B-8`, `B-4` | 4 each | Hash chain, parent, file extension/directory pointer, secondary type, same as classic |

`NaC` begins with the name length byte `n`, then `n` Latin-1 name bytes, then a comment length byte `c`, followed by `c` comment bytes **if space permits**. The contents may occupy no more than 112 bytes in total; overflow comments use a separate block. International case folding and name hashing apply to the **actual NaC name** and are unchanged by LNFS. The AmigaOS API can carry 107 name characters; Hyperion's AmigaOS 3.1.4 format guidance specifies a conservative **106-character interoperable writer limit**. Our format reader should safely handle up to 107 if independently verified, while the writer must cap at **106 bytes** until cross-implementation qualification. Volume/root names remain **30 bytes**.

If the name and full comment cannot both fit, store the comment in a separately allocated and checksum-valid block:

| Comment block field | Offset | Meaning |
| --- | --- | --- |
| Type | 0 | `T_COMMENT = 64` |
| OwnKey | 4 | This comment block's own key |
| HeaderKey | 8 | The directory entry header owning the comment |
| Reserved | 12..19 | Zero |
| Checksum | 20 | Whole-block metadata checksum |
| Comment | 24..103 | 80-byte BCPL comment area: length byte + up to 79 bytes |
| Reserved | 104..B-1 | Zero on new images |

The comment-block key is stored at `B-72` in the owning entry and must be allocated in the normal OFS bitmap. This separate block is **never** a file data block. On rename, comment replacement, deletion and recovery, update/free it consistently. A root directory has **no LNFS NaC field**, only the usual classic 30-byte volume label.

### 13A.2 LNFS root extensions

The DOS6 root uses the classic root layout **except** these two extra fields:

| Offset | Length | LNFS root field |
| --- | --- | --- |
| `B-44` (`w[W-11]`) | 4 | `NumBlocksUsed`: count of allocated blocks recorded in the filesystem bitmap |
| `B-16` (`w[W-4]`) | 4 | `FileSystemType` = BE32 `0x444F5306`; must match the bootblock DOSType |

`NumBlocksUsed` is meaningful only when the bitmap-valid root flag says the bitmap is valid. **Zero** is a sentinel meaning the count must be recalculated, not necessarily that no blocks are allocated. Writes that allocate or free blocks must keep this counter in sync with the bitmap; validation must restore it after bitmap reconstruction. A newly formatted empty volume must record the actual occupied **nonreserved** blocks (root, bitmap pages and bitmap extensions, etc.), not a guessed zero or a device-sector count. An absent or mismatched root `FileSystemType` on a supposed DOS6 volume is a format error, not permission to reinterpret it as DOS0.

### 13A.3 Invariants and conformance tests

- The boot signature is `DOS\6`; the root echo equals `DOS\6`; the metadata checksum validates **including** the two nonzero LNFS root fields.
- The root volume label stays at `B-80` and uses classic 30-byte semantics. Nonroot names use **NaC**, not the old `B-80` 30-byte field.
- The international name hash is identical to DOS2; no DOS4 directory cache is required or permitted by the LNFS mode.
- A short filename and a 79-byte comment may require a separate `T_COMMENT=64` block; test the exact NaC boundary, overflows, allocation, cleanup and corruption.
- Independently create and read 30-, 31-, 106-, and where supported 107-byte names, ensure folded hashes and lookups agree, and refuse embedded colon/slash or invalid lengths.
- Read/write/remount/rename/delete and bitmap-counter change/recovery must be tested with genuine LNFS fixtures, not a re-labelled DOS2 image.
- Implementations without an LNFS-capable namespace reader or writer MUST fail closed for DOS6 rather than report all names as empty or write to the old classic offsets.

These requirements are drawn from the original low-level LNFS author documentation, not guessed from FFS behavior. They must be independently validated on real Amiga-created LNFS media before a product claims full interoperability.


## 14. Required read, write and namespace behaviour

A conforming implementation must preserve this sequence of semantics; storage system calls or UI callbacks are not the format:

**Mount (read-only):** validate partition bounds and geometry, DOS identity, canonical root, root checksum/type/hash size and bitmap metadata as appropriate. A partially degraded volume can be exposed only by an explicit recovery mode with clear diagnostics. Reject every DOSType outside the four OFS identifiers listed in §2, even if its metadata has common AmigaDOS features.

**Lookup / readdir:** follow the exact hash function, variant folding and bucket chains. Enumerate files, directories, recognized links and other supported entries, with deterministic cycle detection. All reported entry sizes and types must reflect metadata and not a cached guess.

**Read / seek:** calculate `ceil(fileLength/P)`, map each logical extent to its OFS data blocks using the pointer table and/or linked list, verify owner/sequence/checksums, return exactly the requested bytes up to EOF, support unaligned cross-block ranges and large files up to the 32-bit length boundary. A reader must not demand the file's whole contents in memory.

**Create / write / append:** allocate metadata/data keys in the bitmap, initialize and checksum each block, update OFS `T_DATA` headers and forward links, the reversed pointer arrays, file extensions, file-length and timestamp fields, directory hash chain, and cache if `DOS\4`. Handle full volume, short write, allocation failure and overwrite without shared-block corruption.

**Truncate / extend:** maintain data-block counts, last `data_size` and terminal `nextData=0` correctly; free surplus keys, pointer extensions and bitmap bits on shrink. When extending, returned bytes between old EOF and new writes must read as zero. Do not expose old freed contents.

**Rename / move:** remove the old hash-chain membership and insert the new one under the new parent/bucket, update entry name and parent and associated metadata; reject duplicate case-folded names and directory cycles, adjust `DOS\4` caches. Commit without orphaning reachable data after failures.

**Delete / unlink / rmdir:** enforce nonempty directory and link ownership rules, remove hash pointers, update checksums, free only blocks genuinely owned by that object, keep hard-linked data live, update bitmaps. Historical unlink often leaves disk block contents in place—**bitmap allocation, not physical erasure, defines free space**.

**Flush, remount, crash recovery:** coordinate data, metadata, bitmap-valid flag and volume timestamps to avoid advertising blocks free while still referenced. OFS has **no generic journal**: do not claim atomic multi-block transactions or full power-loss recovery without demonstrated mechanism. Recovery must detect and report contradictory bitmap references, invalid checksums and unreachable/orphaned blocks.

**[POLICY]** Linux execution or OFS-root boot is an adapter qualification, **not a structural format field**. The driver must correctly expose ELF bytes through Linux's page cache, faults and mmap before removing execution restrictions. UEFI must read the same OFS files through its own ABI. Neither a successful format nor compiling `ofs.ko` proves the complete boot path.

## 15. Format-level errors, limits and interoperability

**Reject or report at least:** incorrect DOSType, unsupported geometry, truncated boot area, impossible root key, invalid block type or secondary type, checksum mismatch, invalid name/comment length, wrong hash bucket, broken parent/self pointers, repeated/cyclic keys, duplicated block allocation, bitmap extension cycles, out-of-bounds bitmap tail, inconsistent two representations of OFS file data, undersized final block, cache record overrun, and arithmetic overflow.

A valid reader **may** support less than the complete format, but MUST disclose the exact subset: DOS variants, 512..32768-byte geometries, link capabilities, write permission, cache support, root execution and address bounds. A successful parser test using its own generator is only a **self-consistency** test. For format conformance, compare at least one independent historical-compatible creator/parser at each geometry it supports and retain known-good / known-bad golden files.

**Performance is not a format requirement:** prefetch, batched I/O, BIO, DMA, page cache, EFI retries and read progress UI belong to adapters. Never weaken checksum, bounds, cycle or allocation invariants to gain speed. A kernel block size constrained by `PAGE_SIZE` is a **driver** limitation, not permission to delete documented OFS format sizes. A 512-entry directory cap is an EFI **safety policy**, not a universal OFS directory limit.

## 16. Acceptance tests for implementations claiming conformance

Each newly supported profile/geometry must pass the following **independent** tests.

| Area | Golden and adversarial cases |
| --- | --- |
| Header/geometry | All seven `B` sizes; even/odd `N`; `R`-aware root placement; authoritative offsets and BE endianness |
| Bootblock | Format selector, nonbootable boot area, boot checksum end-around carry, distinct metadata checksum |
| Root | Root type/subtype/hash, all three timestamps, volume label bounds, bitmap pointers/extensions |
| Hash/name | Empty/collision chains, mixed ASCII, international Latin-1 including `0xE4/0xC4` and `0xF7`, duplicate-folded names, name limits |
| Allocator | Reserved keys, root/bitmap/extensions in-use, free/used bit direction, cross-bitmap allocation, tail bits, full disk, corrupted bitmap and recovery |
| Files | Empty, 1 byte, `P-1`, `P`, `P+1`, `H*P` and `(H+1)*P`; check reversed pointers and OFS `nextData` chain independently |
| Data integrity | Every data header owner/sequence/size/checksum; large kernel/initramfs-sized files; random seek, read-after-write, remount |
| Metadata/namespace | Create, mkdir, collisions, rename across parents, unlink, truncate, symlink/hardlink when claimed, valid protection and timestamps |
| DOS variants | `DOS\0`, `DOS\2`, `DOS\4` and `DOS\6` (NaC, comment blocks and root accounting); reject any identifiers not listed in §2 |
| Large addressing | Boundary around `2^31` and `UINT32_MAX` on sparse disposable media, without signed or byte-offset overflow; reject impossible volume length |
| Errors/recovery | Invalid checksums, swapped BE fields, cycles, torn metadata, simulated I/O failure and corruption on writable media |
| Independent implementations | Test real externally generated Amiga-compatible images; preserve fixture provenance and hashes; independent reader **and** writer checks where supported |
| Platform qualification | Linux mount/read/write/mmap/exec; UEFI mount/lookup/read, DMA alignment and >4 GiB partition-relative offsets; physical media last |

**No physical SD, NVMe, ESP or UEFI NVRAM modification is implied or authorized by this specification.** Destructive partition formatting stays separately guarded until formatter, Linux and EFI reader have passed the identical qualified geometry. Preserve user recovery paths.

## 17. Scope boundary, review discipline and tracked implementation gaps

This Markdown is the **canonical OFS target contract**. The existing `DESIGN.md` defines source ownership and layering; `core/ofs_disk_layout.h` and `core/ofs_primitives.*` implement format primitives; `tools/ofs-image/main.c` is the formatter; `linux/` and `efi/` contain adapters. **They are not normative replacements for the independently sourced filesystem format.**

All changes to code or this document must cite the affected numbered section, state which of `DOS\0/2/4/6` and `B` values it implements, and add a failing-before/passing-after golden test. Do not copy FFS behavior merely because Linux's upstream `affs` source handles both. Track remaining unsigned-capacity, large-block, root-boot and independent Amiga qualification in [issue #27](https://github.com/Infiltrator-Projects/Filesystems/issues/27) and [the separate implementation compliance matrix](../../../docs/OFS_SPECIFICATION_COMPLIANCE.md).

**Known intentional boundary:** This OFS profile does not pretend to canonically specify muFS-specific owner semantics, alternative recovery formats, third-party partition management or EFI/GRUB boot files. Those require their own documented extension profiles. The OFS on-disk format is not an EFI driver interface, Linux VFS ABI or bootloader API.

---

### External source index by section

- [ADFlib FAQ `4.1–4.2](https://adflib.github.io/FAQ/adf_info.html): boot area, root placement, offsets, hashes and block checksums.
- [ADFlib FAQ `4.3](https://adflib.github.io/FAQ/adf_info.html): bitmap bit polarity, extension chain and geometry.
- [ADFlib FAQ `4.4–4.6](https://adflib.github.io/FAQ/adf_info.html): files, reversed pointers, OFS data blocks, directories and links.
- [ADFlib FAQ `4.7, `5.3–5.4](https://adflib.github.io/FAQ/adf_info.html): directory caches, cache record shape and international case folding.
- [ADFlib FAQ `6](https://adflib.github.io/FAQ/adf_info.html): RDB partition separation and logical pointer relativity.
- [Linux AFFS documentation](https://www.kernel.org/doc/html/v6.7/filesystems/affs.html): historical filesystem identities and driver limitations.
- [AmigaDOS technical reference](https://developer.amigaos3.net/sites/default/files/downloads/2024-10/Amiga_ROM_Kernel_Reference_Manual_DOS.pdf): original AmigaDOS interpretation and later variant caveats.

- [Olaf Barthel, official DCFS/LNFS structure specification](https://wiki.amigaos.net/wiki/DCFS_and_LNFS_Low_Level_Data_Structures): authentic DOS6 NaC fields, comment blocks, root used-block count and type echo.

## Historical format evolution, compatibility and provider boundaries

### Origins and release chronology

Commodore International's original AmigaDOS disk filesystem shipped in 1985 with AmigaOS 1.0; the designation OFS (Old or Original File System) was applied retrospectively. It was designed around floppy disks. FFS (Fast File System, also called AFFS in Linux) was introduced with AmigaOS 1.3 in 1988, handler v34. FFS removes the 24-byte per-data-block OFS header, permitting direct transfer of whole payload blocks and increasing capacity and sequential throughput on hard disks. OFS retains type, owner, sequence, length, next-block and checksum fields; FFS raw payload blocks do not. This affects salvage as well as performance.

AmigaOS 1.0–1.2: original DOS0. AmigaOS 1.3 (1988, v34): DOS1 and compatibility with DOS0. AmigaOS 2.0 (1990, v37; international profiles documented for 2.04): DOS2/DOS3, ROM-integrated FFS. AmigaOS 3.0 (1992, v39): DOS4/DOS5. AmigaOS 3.1 (1993, v40/v40.1): last Commodore FFS generation. Later third-party v44 TD64 patches and licensed v45 NSD handler releases (AmigaOS 3.5/3.9, Haage & Partner) extended device addressing.

In the early 2000s, Olaf Barthel's FFS2 evolved the handler for MorphOS (v46) and AmigaOS 4 (v50), including DOS6/DOS7 long-name modes. These are OFS and FFS format variants, not a new unrelated on-disk filesystem. Hyperion's AmigaOS 3.1.4 (September 2018) shipped v46 with multiple large-device interfaces; 3.1.4.1 (July 2019) updated the disk components. AmigaOS 3.2 (May 2021) shipped the v47 generation with further refinements. Version numbers are handler lineage, not the fourth DOSType byte; versions from distinct branches are not a single monotonically ordered release train.

### DOSTypes and namespace compatibility

DOSType is the binary sequence 44 4F 53 xx, not ASCII 'DOS0'. DOS0: original OFS; DOS1: original FFS; DOS2: OFS International; DOS3: FFS International; DOS4: OFS Directory Cache; DOS5: FFS Directory Cache; DOS6: OFS Long Name File System (LNFS); DOS7: FFS LNFS. Even types own OFS header-bearing payload; odd types own FFS raw payload. DOS2/3 use international Latin-1 case folding; DOS4/5 inherit it and add DCFS; DOS6/7 inherit international folding but do not use directory cache. Classic names are limited to 30 on-disk bytes; some sources count 31 including length/terminator. LNFS has a 107-character API envelope; Hyperion recommends a 106-character interoperable writer limit. Volume labels remain 30 bytes. The 112-byte NaC field stores length-prefixed name and comment; overflow comments use type-64 blocks. International case folding is not Unicode folding.

International mode changes equivalence/hashing for accented Latin-1 letters, not the raw character encoding; pre-international handlers could already store high-byte characters but did not consistently fold them. DOS2/3 are not backward-compatible with purely original handlers. DOS4/5 need directory-cache-capable handlers, and DOS6/7 cannot be mounted by older handlers that lack LNFS layout knowledge. Historical compatibility refers to the handler, ROM, and boot path, not just software version. A later handler can read earlier supported types, but the reverse is not guaranteed. DOS0 remained common on commercial floppy games and magazine disks for pre-2.x compatibility; trackloaders may bypass the filesystem entirely.

### Boot, RDB and device integration

Hard disks can carry a Rigid Disk Block (RDB) with partition records (PART), filesystem headers (FSHD) and load segments (LSEG). The partition environment supplies DOSType, block geometry and boot priority; a suitable controller/driver can load a filesystem handler from the RDB before mounting it. Kickstart 1.3 added autoboot support for hard disks and reset-resident RAD: RAM disks. Workbench 1.3 supplied the disk-based FFS handler for RDB installation. Kickstart 1.2 could boot 1.3 from floppy but lacked native FFS hard-disk autoboot. Kickstart 2.x incorporated FFS in ROM; RDB handler loading remained available for replacements and third-party formats.

Unpartitioned trackdisk floppies have no RDB and rely on a ROM-supported DOSType for normal boot. FFS-formatted floppies generally require Kickstart 2.0 for native boot; pre-2.x access could be arranged through installed handlers or unsupported mount workarounds. Directory-cache variants require the AmigaOS 3.0-era handler. DOS types can also be selected via mountlists/DOSDrivers. Application compatibility normally follows AmigaDOS calls; raw trackloaders are outside filesystem semantics.

The project's Common V2 host delivers a bounded, partition-relative volume. RDB partition selection and executable Amiga handler loading are outside that filesystem ABI; a historical bootable disk is not proof of InfiltratorOS BOOT compatibility. GPT/MBR, RDB and floppy/hardfile geometry must not be conflated. Never execute an untrusted handler from media. Native host read/write offsets are checked wide byte offsets.

### Allocation, validation and repair history

OFS and FFS use one or more bitmap blocks, with extension blocks for larger volumes; the root carries a bitmap-valid flag. Historical descriptions of a single-sector bitmap apply only to small media, not the general format. Before mutation the handler marks allocation state invalid; after consistent writes it updates the bitmap and marks it valid. Interruption may leave a dirty bitmap, so validation walks reachable directory/header/extension/data ownership and reconstructs allocation. This is not an atomic journal and cannot guarantee that only the most recently modified data is lost.

Historic disk-validator operation can make volumes read-only while validation runs; scans and access slowdowns grew severe on larger disks and could last hours. If the validator fails, recovery may require a salvage tool or copying recoverable files to another volume. Earlier boot disks supplied a disk-resident validator; malware such as the historically named Saddam Hussein virus exploited replaceable validator code. ROM integration in Kickstart 2.x reduced that replacement vector. These historical details are context, not a guarantee against all malware.

Commodore supplied DiskDoctor in earlier releases and later removed it from the standard disks; DiskSalv was a prominent third-party repair, undelete and quick-format recovery utility. Defragmentation and conversion require exclusive ownership or a locked volume; normal access is restricted while a maintenance tool relocates blocks. Tools with legacy device interfaces can misaddress storage beyond their safe boundary and cause damage. Do not equate a successful bitmap rebuild with file undelete, unformat, or repair of contradictory metadata.

### Addressing limits and large media

Classic trackdisk CMD_READ/CMD_WRITE use 32-bit byte offsets, creating a 4 GiB (2^32 bytes) device-address boundary. Some old handlers, tools and signed arithmetic imposed a 2 GiB boundary; traditional AmigaDOS APIs also exposed a roughly 2 GiB signed file-size limit. The historically reported 127 GB partition ceiling depended on handler, DOS library, geometry and tooling, and is not a universal property of every DOS type. Old code may wrap byte addresses and overwrite earlier storage rather than fail. Never treat a legacy 4 GB figure as the canonical on-disk volume limit.

TrackDisk64 (TD64) third-party extensions used 64-bit device offsets, often with v44 handler patches and Phase5/controller packages. New Style Device (NSD) provided a distinct 64-bit command convention in the v45 licensed AmigaOS 3.5/3.9 FFS line. The schemes were not automatically interchangeable; a driver, filesystem and repair tool must agree on the interface or have a suitable translation layer. Hyperion v46 supports TD64, NSD and classic commands (and direct SCSI where appropriate); large-media operation still requires a compatible controller and correct RDB geometry. Later handlers can support partitions beyond 2 GiB and media beyond 4 GiB; do not infer that every old utility can.

On disk, unsigned 32-bit block keys and 32-bit file lengths are separate from host byte offsets. For B=512, 32-bit block addressing is just below 2 TiB; for B=32768 it is just below 128 TiB. These are arithmetic bounds, not historical or tested volume limits. A UINT32_MAX byte-length field is not a promise that a classic AmigaDOS application supports a 4 GiB file. Modern providers must use overflow-checked partition-relative arithmetic, test large offsets and distinguish format limits from provider and device limits.

### Directory caching, later formats and ecosystem

DCFS stores auxiliary directory-list records in addition to authoritative hashed directory entries. It reduces scattered metadata reads and can speed directory enumeration, not ordinary file payload reading. Create, delete, rename and metadata updates must also update cache records, adding writes and consistency obligations. Olaf Barthel's documented limitations include lack of consolidation/garbage collection for partly filled cache blocks: partially used pages may persist and waste space. Floppy directories may benefit from lower seek cost but the cache also consumes scarce floppy space; larger hard disks can see net degradation from cache maintenance. Do not infer all implementations exhibit identical performance.

LNFS substitutes long-name/comment metadata and a root used-block counter/type echo, not DCFS. It retains bitmap validation and does not add transactional crash rollback. FFS2 does not remove the underlying validation requirement. Both original formats have Amiga protection flags, timestamps and file comments (filenotes); neither provides transparent compression or encryption as an original format feature. AmigaOS, MorphOS and AmigaOS 4 have supported FFS; SFS and PFS emerged as alternatives in the mid-1990s, with PFS3 and SFS2 having distinct on-media semantics. SFS has been used as a default filesystem in AROS distributions; defaults can vary by distribution/version.

Linux's AFFS driver is an independent implementation, not an authority for our exported capabilities. Its supported DOSTypes and read/write limitations vary by version; historical descriptions claiming universal DOS0–5 formatting or writable DCFS overstate what a kernel driver guarantees. Formatting is normally a separate user-space tool. The Amiga xfs freeware utility could access older OFS/FFS among other formats, but predates LNFS and did not provide general formatting. ADFlib, amitools and independent Amiga media are valuable interoperability references; third-party compatibility must be demonstrated rather than assumed.

### Sources and interpretation

Historical comparison: Amiga Fast File System article (user-supplied 2026-11-25), https://en.wikipedia.org/wiki/Amiga_Fast_File_System . The article is a secondary source; its unsourced characteristics and broad compatibility statements are not normative. Consult original manuals and handler-version documentation for device-specific details.

Original format: AmigaDOS ROM Kernel Reference Manual (DOS), ADFlib disk-format FAQ https://adflib.github.io/FAQ/adf_info.html ; Olaf Barthel, DCFS and LNFS Low Level Data Structures https://wiki.amigaos.net/wiki/DCFS_and_LNFS_Low_Level_Data_Structures ; Linux AFFS documentation https://www.kernel.org/doc/html/v6.7/filesystems/affs.html .

Hyperion large-media FAQ https://www.hyperion-entertainment.com/index.php/frequently-asked-questions/55-amigaos-314/202-can-i-use-partitions-beyond-the-4gb-boundary ; Hyperion 3.1.4.1 release notice https://www.hyperion-entertainment.com/index.php/news/1-latest-news/280-update-to-amigaos-314-released ; AmigaOS 3.2 product documentation https://www.hyperion-entertainment.com/index.php/where-to-buy/direct-downloads/327-amigaos-32 . Historical chronology: https://www.gregdonner.org/workbench/ ; Thomas Rapp, FAQ about the 4GB problem, https://thomas-rapp.hier-im-netz.de/4gb.html .

These references describe the historical formats and Amiga handler ecosystem, not certification of our provider. The current exported Common V2 operation masks, implementation and exact-revision tests are authoritative for InfiltratorOS. The project's canonical filesystem implementations remain independent.

### OFS provider capability boundary and acceptance

Current Common V2 source in native/filesystems/ofs/module/ofs_common.c accepts formatter selectors DOS0, DOS2, DOS4 and DOS6; the core descriptor declares probe, mount, stat, enumeration, read, write, create, remove, rename and truncate, with volume query, label, sync, check and format. Native OFS retains B-24 payload, checksummed data records and format-specific directory/cache/LNFS semantics. Only exported operation groups and masks are authoritative; a private native function or older adapter is not an extra public capability.

The detailed OFS_SPECIFICATION.md and qualification matrices define exact structures, bounds and independent tests. Historical claims in old Linux/EFI adapter records are not the current production deployment contract. A documented 107-byte LNFS reader fixture does not imply a 107-byte interoperable writer; the writer limit is 106. Actual physical-media durability, every failure sequence and every original handler combination require separate qualification. A successful compilation does not prove runtime or BOOT integration. Use only the Common V2 provider as the filesystem authority.


## Worked OFS file and extension mapping

For a 512-byte filesystem block, H=72 header data pointers and P=488 payload bytes. A 40000-byte file requires ceil(40000/488)=82 data blocks: 72 referenced in the header and ten in one extension block. Data pointer j in either table is at B-204-4*j (j starts at zero); select header or extension by quotient and remainder of logical payload block / H. Logical byte 35000 belongs to logical data block 71, offset 352, so read from byte 376 of the referenced OFS block after its 24-byte header. Compare the header's reversed pointer table with each data block's owner, sequence, payload size and next pointer. The final data block holds 472 bytes. Validate the full-block metadata checksum; do not treat an FFS raw block as OFS.

## Directory collision and allocation exercise

For a conventional 1760-block floppy, B=512 and reserved count 2, root is block 880 and one bitmap covers the remaining 1758 blocks. The bitmap has 4064 usable bits after its checksum. Resolve two names hashing to one bucket by traversing their hash chain, comparing native folded bytes and confirming parent ownership. Locate data blocks through their own pointers, not the directory chain. Reserve allocation and verify checksums before publishing a new file. For DOS4 maintain cache coherence, for DOS6 maintain LNFS name/comment and used-block accounting; those are separate formats. Fault-inject each update boundary and compare against independently created AmigaOS fixtures.
