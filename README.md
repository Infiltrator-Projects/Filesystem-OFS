# Amiga Original File System — OFS.so

OFS provides a single x86-64 shared object with a freestanding native
reader. A Common V2 host supplies sector I/O and allocation callbacks.

`OFS.so` exports Common V2 for filesystem operations, alongside the
dedicated relocation and recovery interface. It handles OFS `DOS0`, international
`DOS2`, directory-cache `DOS4`, and long-name `DOS6`. OFS data always retains
its 24-byte data header and checksum. Amiga FFS is a separate filesystem.

| Surface | Implemented behavior |
| --- | --- |
| Geometry | 512, 1024, 2048, 4096, 8192, 16384 and 32768-byte filesystem blocks; unsigned 32-bit block keys and 64-bit volume byte offsets; explicit reserved/root geometry in V3 |
| Files | Read, random overwrite, append, zero-filled growth, truncate, reversed header pointer tables and multiple extension blocks; 32-bit file lengths |
| Namespace | Lookup, enumeration, create, mkdir, rename/move/replace, unlink/rmdir, file/directory hard links with promotion and native Amiga symbolic targets |
| Metadata | Latin-1 on disk/UTF-8 at the API, DOS-specific case folding, protection bits, uid/gid, comments, DateStamps, labels and allocation space |
| DOS4 | Cache creation, validation and updates after namespace, file-size and metadata changes |
| DOS6 | Long names, inline/overflow comments and allocated-block counter maintenance; 106-byte writer/107-byte reader name limits |
| Volume management | Explicit unmounted format, whole-volume ownership/bitmap verification and explicit bitmap recovery after a structurally valid scan |
| Boot | Read-only streaming, directories, seeks, filenames and labels; firmware-compatible Common V2 profile; no runtime library imports or constructors |
| Defragmenter | Analyse/map, metadata and data relocation, exact 10% Growth reserve, verified staging, durable journals, Stop and bound-target Recover; filesystem semantics stay in OFS.so |

V3 is serialized and owns one mount per loaded image. The host provides
bounded partition-relative I/O, allocation, flush and optional native Amiga
time callbacks. Its public contract is
[`ofs_v3.h`](native/filesystems/ofs/module/ofs_v3.h). Applications use Common V2 for ordinary filesystem operations. File and user-directory hard links support
creation, mutation through aliases, deletion and promotion. Explicit directory
links may form cycles; traversal is bounded and whole-volume verification visits
each original once. Removing the sole rooted name of a cyclic directory tree
is refused before writes, preserving reachability. The volume root itself is
not a user directory and cannot be hard-linked.

Build and test the portable and CPU-native modules on x86-64 Linux.

```sh
make check
make native
make release
```

The Debian package installs `/System/Filesystems/OFS.so` and
`/System/Resources/Documentation/Filesystem-OFS.pdf`. The native RUN builds
the same source and uses measured two-pass profile-guided optimisation.
The installed module has no dynamic library imports. Its second exported
entry point preserves the dedicated V2 relocation and recovery interface
used by Defragmenter.

The read-only media test uses an independent Amiga image producer. It
compares files from an OFS volume against their originals through Common
V2 on 512-byte and 4096-byte hosts, exercises checksum failures, and verifies
that the image stays byte-for-byte unchanged. Acceptance by the rebuilt
BOOT loader and a kernel handoff from this module in OVMF remain open.

Earlier format and interoperability work is recorded in
[MIGRATION-PROVENANCE.md](MIGRATION-PROVENANCE.md).
