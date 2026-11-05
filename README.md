# Amiga Original File System — OFS.so

Independent OFS filesystem module for InfiltratorOS, with the same shared
object used by applications and Boot. Filesystem implementation lives here;
Boot supplies its generic ELF loader, byte I/O and UEFI filesystem bridge.

`OFS.so` exports the preserved V1/V2 interfaces, a full read/write **V3** API,
the generic **Boot V1** interface and process-hosted **Defrag V2**. It handles OFS `DOS0`, international
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
| Boot | Read-only streaming, directories, seeks, filenames and labels; exact generic ABI/manifest; no runtime library imports or constructors |
| Defragmenter | Analyse/map, metadata and data relocation, exact 10% Growth reserve, verified staging, durable journals, Stop and bound-target Recover; filesystem semantics stay in OFS.so |

V3 is serialized and owns one mount per loaded image. The host provides
bounded partition-relative I/O, allocation, flush and optional native Amiga
time callbacks. Its public contract is
[`ofs_v3.h`](native/filesystems/ofs/module/ofs_v3.h). Existing V1/V2 clients keep
their 512-byte DOS0/2/4 contract; V2 remains bounded existing-file overwrite.
New filesystem operations use V3. File and user-directory hard links support
creation, mutation through aliases, deletion and promotion. Explicit directory
links may form cycles; traversal is bounded and whole-volume verification visits
each original once. Removing the sole rooted name of a cyclic directory tree
is refused before writes, preserving reachability. The volume root itself is
not a user directory and cannot be hard-linked.

Build and qualify on x86-64 Linux:

```sh
make check
python3 tests/fetch-oracle.py --output .qualification/oracle
make interop AMITOOLS_SOURCE=.qualification/oracle/amitools-0.8.1
make native
make release
```

The release contains portable `OFS.so`, its DEB, a source-first native RUN
installer, and separately qualified `ofs-linux-6.12.107.ko`. The published
DEB installs `/System/Filesystems/OFS.so`, the manual at
`/System/Resources/Documentation/Filesystem-OFS/OFS.pdf`, and the qualified
module at `/System/Kernel/Filesystems/ofs.ko` for manual use. The native profile
uses `-O3 -march=native -mtune=native -flto=auto` with measured two-pass PGO;
AVX/AMX instructions are disabled for firmware compatibility. Build records
include flags, profile and the installed module's SHA256. Installer tests use
private RAM-backed images when `/dev/shm` has at least 32 MiB free. The images
exercise the same workloads and injected I/O failures. Hosts without that
space use the normal temporary directory, which the full `make check` suite
also uses.

The manual RUN builds `OFS.so` and `ofs.ko` locally, generates the PDF from
`docs/OFS-manual.pages`, and installs all three files at the same paths as the
portable DEB. It uses the running kernel's prepared build directory under
`/lib/modules` by default. Install the matching kernel headers first, or pass
`--kernel-build DIRECTORY` to target a prepared kernel build explicitly. The
module is stored for manual use. Use `--build-only` to produce the package
without installing it.

```sh
bash filesystem-ofs-0.1.9-native-installer.run --build-only --output ./native-package
```

InfiltratorOS uses `--system-package-mode` to build the SO and PDF as a package.
Its installer checks and stages the matching released kernel module separately.

Defragmenter loads `/System/Filesystems/OFS.so` for analysis, Defrag, Growth
Defrag and Recover. The same file supplies V3 formatting and creation for its
Test Media utility. A missing or incompatible module stops those operations.
The SO owns the placement and journal formats; the process host supplies generic
target safety, byte I/O, durable files, hashing and Stop.

Build the native kernel adapter separately, without installation:

```sh
make kernel KERNEL_BUILD=/path/to/prepared/kernel/build
```

The shipped reference KO is specific to Linux 6.12.107. It compiles the same
canonical engine as the SO and supports read/write DOS0/2/4/6 at all seven
block sizes, including 8, 16 and 32 KiB on a 4 KiB-page kernel. Its VFS binding
provides buffered I/O, executable/private/shared mappings, native metadata
xattrs, hard-link lifetimes and deferred deletion of open files. Each mounted
volume has independent state. Qualification also builds and exercises the
Linux 7.0 VFS binding. Build a separate KO against the headers for the exact
kernel that will load it. Kernel SO loading is a later generic mechanism.

For Boot **v0.1.38**, stage the same portable SO bytes as `OFS.so` in
`filesystems_x64/` beside Boot's EFI executable. A manual entry can require
`filesystem_module ofs`, reference a volume label and load `\vmlinuz` and
`\initrd.img` with `linux_load memory`. The OFS volume is partition-relative;
RDB/MBR partition discovery belongs to the host. Boot refuses unsigned modules
when Secure Boot is enabled or its state cannot be established. These builds
and tests do not deploy modules to an ESP or change firmware settings.
The combined OVMF qualification loads Linux and its initramfs from OFS,
loads the matching `ofs.ko`, switches to that OFS volume as `/`, and verifies
a Linux write on the resulting disk image.

The evidence and its limits are recorded in
[`docs/SO_COMPLETION.md`](docs/SO_COMPLETION.md) and
[`MIGRATION-PROVENANCE.md`](MIGRATION-PROVENANCE.md). Qualification includes an
independent Amiga-compatible producer/consumer and real Linux handoff through
Boot in OVMF. Native Amiga hardware/handler interoperability remains a separate
qualification gate. The standalone EFI adapter is retained for compatibility;
Boot uses the shared module. This release does not install a Linux kernel or Windows driver.
The earlier [DOS0 platform completion matrix](docs/OFS_DOS0_COMPLETION_MATRIX.md)
retains those outstanding platform gates.

Writable mounts verify ownership and allocation before enabling writes.
The Linux driver checks file payload checksums and links as it reads or
writes those blocks. The userspace V3 writable mount and explicit verifier
still check file payloads across the whole volume.
Overwrites validate the changed range and its neighboring links. A V3
write cache miss still checks the whole chain before changing the file.
Explicit dirty mounts allow inspection/recovery until a successful recovery
scan. Writes follow OFS's bitmap-valid protocol. Failed writes/flushes are reported;
there is no journal or atomic rollback promise. Recovery rebuilds allocation
only when the namespace and data ownership are consistent. Contradictory
metadata is reported rather than guessed into a valid filesystem.

Source origin: Filesystems commit
`7473534b1afbd1e9b14f8ab9ae0769fc6be2126e`, independent OFS baseline
`386d97969996864a939bd644c500cd6394a348a3`. Copyright notices are preserved.
License: GPL-3.0-or-later.
