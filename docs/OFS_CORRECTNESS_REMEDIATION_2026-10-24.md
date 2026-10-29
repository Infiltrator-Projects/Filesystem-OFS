# OFS specification conformance: verified findings and remediation register

Date: 24 October 2026  
Status: **OPEN — documentation-first record**. Recorded before remediation changes.  
Baseline: Filesystems `main` commit `92e1a43f61e1b6964224791ed5514355ff41bd15`; Linux `ofs.ko`, image formatter, and experimental EFI OFS reader are separate qualification surfaces.

## Authority and interpretation

- *Amiga ROM Kernel Reference Manual: DOS*, section 13.6.4 (The Root Block), section 13.6.11 (Data Block), section 13.6.12 (Bitmap Block): https://developer.amigaos3.net/sites/default/files/downloads/2024-10/Amiga_ROM_Kernel_Reference_Manual_DOS.pdf
- Linux AFFS documentation: https://www.kernel.org/doc/html/v6.7/filesystems/affs.html
- This is **not** a blanket claim of compatibility with all original Amiga devices or all variants of the filesystem. Distinguish on-media mathematical limits from current Linux/UEFI runtime support. Do not infer OFS layout from the FFS data path.
- The manual describes FFS structures, many shared with OFS; OFS's 24-byte per-data-block header is specifically different from FFS. Independently verify each supported variant before claiming compatibility.

## Recorded defects and scope gaps (before fixes)

| ID | Finding and original source | Impact | Required proof and disposition |
|---|---|---|---|
| OFS-01 | `tools/ofs-image/main.c`: creator and verifier use `root = blocks / 2`. The default Linux root candidate is `(reserved + blocks - 1) / 2` with `reserved=2` in `native/filesystems/ofs/linux/lifecycle.c`. | Odd total-block geometries differ by one block; independently mounted images may be undiscoverable. Existing formatter tests use even-block MiB geometries. | Use a single root-placement rule, verify both odd and even block totals with an independently constructed root fixture, and test all seven block sizes. Preserve a read-only, explicitly documented compatibility path for legacy images if needed. |
| OFS-02 | `native/filesystems/ofs/linux/affs_compat.h` stores `s_partition_size` as signed `int`; `lifecycle.c` rejects `block_count > INT_MAX` and restricts blocksize to `PAGE_SIZE`. | Linux adapter cannot mount the theoretical full on-disk address range or 8/16/32 KiB blocks on 4 KiB PAGE_SIZE hosts. | Audit *all* conversions, bitmap arithmetic and I/O interfaces; test 2^31 boundary, near 2^32, corruption, mount and write on disposable images. Never simply relax integer checks. |
| OFS-03 | `native/filesystems/ofs/efi/reader.c`, `reader.h` hardcode 512-byte blocks, 488-byte payloads, 72 hash entries and `DOS\\0`. | Does not read larger-block OFS or OFS International/DirCache variants. | Blocksize-aware reader, offset-safe firmware I/O, independent 512..32768 byte fixtures; prove DOS\\2 case folding and read-only DOS\\4 cache semantics, with FFS rejected. |
| OFS-04 | `tools/ofs-image/main.c` physical formatting remains 512-byte only and <2^31 blocks. Image mode already supports 512..32768 and unsigned 32-bit block metadata. | Device formatter is intentionally gated; image formatter's successful self-verification is not external interoperability proof. | Keep physical guard until both qualified Linux and EFI readers can read that exact geometry; verify images with an independent parser/implementation. |
| OFS-05 | `lifecycle.c` sets `SB_NOEXEC` on Linux OFS, as upstream AFFS documents for OFS's non-mappable 488-byte data-block payload. | Ordinary Linux execution from an OFS root is blocked, even if UEFI reads the kernel. **Removing SB_NOEXEC alone is not a valid fix**: Linux mmap/page-fault and executable code loading would still require an independently proven mapping model. | Prove an executable-access architecture (e.g. copy-up/ram-backed executable root, or fully validated OFS executable fault handling) and boot+initramfs root handoff before changing safety semantics. |
| OFS-06 | Partial read-only EFI driver has bounded directory scan and file-size limitations; 0.5.76 formatter supports only empty freshly created volumes and checks its own output. | Not complete OFS interoperability, recovery, hard-link and variant coverage. | Read/write/remount/crash tests on diverse real-world and independently built media; explicitly enumerate supported and unsupported features. |

## Verified rules and implementation boundaries

- For a partition with `N` logical filesystem blocks and the traditional `reserved=2`, the normal root key is `floor((2 + N - 1)/2)`; do **not** silently substitute `floor(N/2)` for odd `N`. Arithmetic must use wide intermediates.
- 32-bit block references imply an *arithmetic* upper count around `UINT32_MAX` blocks; this is **not** a validated physical capacity. An OFS data block is **block_size - 24 bytes** of file content, unlike FFS.
- Default `DOS\\0` and international `DOS\\2` and directory-cache `DOS\\4` require distinct tests, including invalid inputs; FFS `DOS\\1/3/5` must never be mistaken for OFS.
- The existing Dell physical SD experiment is **paused**. No operation on real media, ESP, UEFI NVRAM or the NVMe test partition is authorized by this branch. Keep Boot0004 recovery.

## Completion criteria

1. OFS-01 fixed and regression-tested for odd and even geometries, with no change in existing 512-byte even-block output.
2. Independent format tests validate root/tail/hash/bitmap/data layout at 512, 1024, 2048, 4096, 8192, 16384, 32768 bytes.
3. OFS-02 through OFS-06 closed **individually** only after their respective Linux, EFI, boot, and destructive-target safety tests pass.
4. All CI workflows green and no unsupported full-specification, root-boot, or 2-TiB physical qualification claims.

Record actual test run links, versions, devices and exceptions when promoting each item; never treat a green build alone as specification certification.
