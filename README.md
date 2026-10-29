# Amiga Original File System — OFS.so

Independent OFS filesystem module for InfiltratorOS, with the same shared
object used by applications and Boot. Filesystem implementation lives here;
Boot supplies its generic ELF loader, byte I/O and UEFI filesystem bridge.

`OFS.so` exports the preserved V1/V2 interfaces, a full read/write **V3** API,
and the generic **Boot V1** interface. It handles OFS `DOS0`, international
`DOS2`, directory-cache `DOS4`, and long-name `DOS6`. OFS data always retains
its 24-byte data header and checksum. Amiga FFS is a separate filesystem.

| Surface | Implemented behavior |
| --- | --- |
| Geometry | 512, 1024, 2048, 4096, 8192, 16384 and 32768-byte filesystem blocks; unsigned 32-bit block keys and 64-bit volume byte offsets; explicit reserved/root geometry in V3 |
| Files | Read, random overwrite, append, zero-filled growth, truncate, reversed header pointer tables and multiple extension blocks; 32-bit file lengths |
| Namespace | Lookup, enumeration, create, mkdir, rename/move/replace, unlink/rmdir, file hard links with promotion and native Amiga symbolic targets |
| Metadata | Latin-1 on disk/UTF-8 at the API, DOS-specific case folding, protection bits, uid/gid, comments, DateStamps, labels and allocation space |
| DOS4 | Cache creation, validation and updates after namespace, file-size and metadata changes |
| DOS6 | Long names, inline/overflow comments and allocated-block counter maintenance; 106-byte writer/107-byte reader name limits |
| Volume management | Explicit unmounted format, whole-volume ownership/bitmap verification and explicit bitmap recovery after a structurally valid scan |
| Boot | Read-only streaming, directories, seeks, filenames and labels; exact generic ABI/manifest; no runtime library imports or constructors |

V3 is serialized and owns one mount per loaded image. The host provides
bounded partition-relative I/O, allocation, flush and optional native Amiga
time callbacks. Its public contract is
[`ofs_v3.h`](native/filesystems/ofs/module/ofs_v3.h). Existing V1/V2 clients keep
their 512-byte DOS0/2/4 contract; V2 remains bounded existing-file overwrite.
New filesystem operations use V3. File hard links can be created; directory
hard-link creation is deliberately refused. Existing directory aliases can
be read with bounded traversal.

Build and qualify on x86-64 Linux:

```sh
make check
python3 tests/fetch-oracle.py --output .qualification/oracle
make interop AMITOOLS_SOURCE=.qualification/oracle/amitools-0.8.1
make native
make release
```

The release contains a portable `OFS.so`, its DEB, and a source-first native
RUN installer. The DEB owns `/System/Filesystems/OFS.so`. The native profile
uses `-O3 -march=native -mtune=native -flto=auto` with measured two-pass PGO;
AVX/AMX instructions are disabled for firmware compatibility. Build records
include flags, profile and the installed module's SHA256. The RUN can build a
package without installing it:

```sh
bash filesystem-ofs-0.1.0-native-installer.run --build-only --output ./native-package
```

For Boot **v0.1.38**, stage the same portable SO bytes as `OFS.so` in
`filesystems_x64/` beside Boot's EFI executable. A manual entry can require
`filesystem_module ofs`, reference a volume label and load `\vmlinuz` and
`\initrd.img` with `linux_load memory`. The OFS volume is partition-relative;
RDB/MBR partition discovery belongs to the host. Boot refuses unsigned modules
when Secure Boot is enabled or its state cannot be established. These builds
and tests do not deploy modules to an ESP or change firmware settings.

The evidence and its limits are recorded in
[`docs/SO_COMPLETION.md`](docs/SO_COMPLETION.md) and
[`MIGRATION-PROVENANCE.md`](MIGRATION-PROVENANCE.md). Qualification includes an
independent Amiga-compatible producer/consumer and real Linux handoff through
Boot in OVMF. Native Amiga hardware/handler interoperability remains a separate
qualification gate. The existing Linux VFS and standalone EFI adapters are
transitional; this release does not install a Linux kernel or Windows driver.
The earlier [DOS0 platform completion matrix](docs/OFS_DOS0_COMPLETION_MATRIX.md)
retains those outstanding platform gates.

Writable mounts verify ownership and allocation before enabling writes.
Explicit dirty mounts allow inspection/recovery until a successful recovery
scan. Writes follow OFS's bitmap-valid protocol. Failed writes/flushes are reported;
there is no journal or atomic rollback promise. Recovery rebuilds allocation
only when the namespace and data ownership are consistent. Contradictory
metadata is reported rather than guessed into a valid filesystem.

Source origin: Filesystems commit
`7473534b1afbd1e9b14f8ab9ae0769fc6be2126e`, independent OFS baseline
`386d97969996864a939bd644c500cd6394a348a3`. Copyright notices are preserved.
License: GPL-3.0-or-later.
