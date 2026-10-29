# AmigaDOS OFS firmware reader and UEFI driver

**Owner: Filesystems.** Boot never embeds or compiles OFS algorithms.

This directory contains two distinct layers:

- `reader.{c,h}`: portable, freestanding, read-only DOS\\0 OFS parser using the canonical Filesystems core. The shared `ofs_core` and `ofs_primitives` remain authoritative for on-disk rules.
- `uefi_driver.c`: standalone x86-64 UEFI Driver Binding and Simple File System adapter, with explicit read-only open, read, enumerate and metadata behaviour. It speaks standard EFI protocols and has no dependency on Infiltrator Boot code.

Build on Ubuntu with `gnu-efi`, GCC and binutils:

```bash
bash scripts/build-ofs-efi.sh dist-ofs-efi
```

The sole firmware payload is `dist-ofs-efi/OFS_x64.efi`, verified as a PE32+ x86-64 EFI boot-service driver (subsystem 11). The script never installs to the ESP or writes firmware variables. Infiltrator Boot loads external filesystem drivers from `drivers_x64/` **beside the currently running Boot executable** (for an `EFI/BOOT/BOOTX64.EFI` layout, that is `EFI/BOOT/drivers_x64/`). Use the actual existing installation path, not a new or duplicate Boot tree. The Filesystems Debian release stages this driver under `/usr/lib/filesystems/efi/experimental/`. It remains experimental and is never automatically installed into Boot's active driver directory.

**OVMF firmware regression:** `scripts/test-ofs-uefi-ovmf.sh` boots a disposable GPT disk under QEMU/OVMF. A UEFI application loads the actual standalone OFS driver, requests Driver Binding via `ConnectController`, opens `\\boot\\kernel.efi` through EFI Simple File System and checks the five-byte test file. A second test partition uses an FFS DOS type and must not bind as OFS. The dedicated CI lane builds the normal release-style driver independently of an opt-in serial-instrumented driver and archives its stage log. Run `OFS_EFI_TRACE=1 bash scripts/build-ofs-efi.sh dist-ofs-efi-diagnostic` only for virtual diagnostics; the normal driver does not contain the serial tracing code. Both the serial-instrumented diagnostic build and the standard uninstrumented EFI binary must pass the OVMF acceptance cases. The positive case also tests protocol teardown by DisconnectController and successful reconnection. The harness does not run Infiltrator Boot, transfer a real EFI-stub kernel, exercise a physical SD controller or establish a firmware I/O timeout on the Dell.

**Qualification:** experimental. CI compilation and native portable-reader tests do **not** prove OVMF firmware binding, full kernel-file loading, media read integrity on the physical Dell Latitude 5550, or Secure Boot compatibility. A live EFI read test is mandatory before a production rollout. The UEFI adapter rejects unsupported/malformed volumes and writes nothing.

**Firmware probe boundary:** only UEFI logical-partition BlockIo handles are eligible. Whole-disk handles and incompatible logical blocks are rejected before firmware disk reads. Each format probe reads at most two 512-byte sectors (boot block and root block); the start path repeats validation and may also read the volume label. A synchronous firmware `ReadDisk()` cannot be interrupted by this driver, so the I/O count is bounded but firmware latency is not. Directory lookup and enumeration stop after 512 entries, rejecting cyclic or overlong chains instead of walking a volume-sized chain. This is a boot-reader limit, not a filesystem format limit.

**Unsigned block addressing audit:** The parser and UEFI partition probe no
longer impose the Linux AFFS adapter's signed 31-bit block-count ceiling on
512-byte OFS media. The accepted **unsigned 32-bit** count remains checked
against the firmware's 64-bit LastBlock before narrowing; standalone
reader tests exercise synthetic metadata pointers above 2^31. This is
**not** a real >1-TiB kernel-file transfer test, a larger block-size
reader, or physical-media qualification. The EFI reader still accepts only
512-byte OFS filesystem blocks; the Linux driver and physical formatter
retain their separate safety gates. See docs/OFS_SPECIFICATION_COMPLIANCE.md.

**Initial scope:** partition-relative DOS\\0, DOS\\2 (international), and read-only DOS\\4 (directory-cache ignored in favor of authoritative hash chains); 512-byte logical sectors and 512-byte OFS filesystem blocks; names up to 30 Latin-1 bytes with DOS-type-correct case-insensitive lookup, linked 488-byte OFS data blocks, files at most 4 GiB. No Amiga RDB whole-disk partition extraction, links, writable EFI operations or general Linux root qualification. EFI-stage readers and Linux `ofs.ko` have different ABIs; Linux requires its own kernel module (often inside the initramfs) to mount OFS after EFI handoff.

## Physical-media, read-only firmware-reader reproduction

Boot 0.1.25 on a Dell Latitude 5550 discovered OFSBOOT and Linux images on a
119 GiB OFS SD card, but UEFI enumeration subsequently returned
`EFI_VOLUME_CORRUPTED` while scanning `\\boot`. That status is a firmware
adapter's general conversion of the canonical reader's error and **does not
prove actual media corruption**. Existing CI exercises a single nested small
file, not a populated full-size physical volume.

Use this diagnostic on Linux with the OFS partition mounted or unmounted. It
opens the partition **O_RDONLY** and traverses the *same canonical OFS EFI
reader* used by the firmware adapter; it does not write, format, repair, or
change any EFI or boot configuration. It reports the directory, hash bucket,
entry count and sector reads. A passing result does not prove UEFI DiskIo
reliability, and a failing result distinguishes a host-reproducible reader
rejection from a firmware-only failure, without claiming why an individual
sector was rejected.

From the Filesystems repository root:

```bash
cc -std=c11 -O2 -Wall -Wextra -Werror \
  tests/ofs_efi_media_probe.c \
  native/filesystems/ofs/efi/reader.c \
  native/filesystems/ofs/core/ofs_core.c \
  native/filesystems/ofs/core/ofs_primitives.c \
  -o /tmp/ofs-efi-media-probe
sudo /tmp/ofs-efi-media-probe /dev/mmcblk0p1
```

The probe performs metadata enumeration only; it is not a full filesystem
checker and cannot establish the integrity of every kernel or initramfs block.

## 0.5.47: EFI I/O versus metadata failures

The Dell Latitude 5550 reported `Volume Corrupt` both while scanning
`/boot` and while loading `vmlinuz` through Boot 0.1.25 with the 0.5.46
OFS EFI driver. The real 249,733,120-sector OFSBOOT partition passed
read-only Linux-hosted enumeration using this repository's own canonical
EFI reader (three entries at root, four under `/boot`). That evidence
isolates no firmware defect by itself.

Previously, the adapter translated **all** canonical reader failures into
`EFI_VOLUME_CORRUPTED`, even when `EFI_DISK_IO.ReadDisk` returned a transport
error or a removable device disappeared. The adapter now preserves an EFI
transport error within each synchronous filesystem operation and only
reports `EFI_VOLUME_CORRUPTED` when the on-disk format checks themselves
reject the metadata. If DiskIo labels a low-level read failure
`EFI_VOLUME_CORRUPTED`, the adapter exposes it as `EFI_DEVICE_ERROR` to
avoid falsely asserting a corrupt filesystem. Neither Boot nor OFS writes
to the SD card, performs repair or retries a failing read.

This is **diagnostic correctness**, not a qualified OFS boot fix. CI
builds and exercises the EFI driver but cannot prove the Dell's physical
firmware DiskIo implementation or kernel handoff succeeds. The 0.5.46
executable already installed on a physical machine is not automatically
replaced by changes to this repository.


## Physical Dell BlockIo transport change — 0.5.49

On 21 October 2026, Dell Latitude 5550 firmware bound OFSBOOT and
successfully read eight consecutive 64 KiB chunks of a 17,029,512-byte
kernel. The next EFI file-protocol Read (starting at offset 524,288) returned
EFI_DEVICE_ERROR. This is a **failing transfer start offset**, not proof
that the exact disk sector is corrupt. The same source passed a complete
Linux-hosted canonical-reader comparison.

The driver formerly used DiskIo.ReadDisk for each aligned 512-byte sector.
It now uses partition-relative BlockIo.ReadBlocks with a single reusable,
Media.IoAlign-compliant sector buffer, avoiding the arbitrary-byte DiskIo
transport layer. Supported() releases its probe buffer, and Start failure,
successful Stop and protocol-install errors release the controller buffer.
DiskIo remains claimed BY_DRIVER: the partition driver may already own
BlockIo, and claiming it is known to fail under OVMF. Filesystem parsing,
error attribution and read-only semantics are unchanged.

On an actual file-read failure from BlockIo, the driver prints the raw
firmware status and partition-relative failing sector. The transport change
and diagnostic require physical Dell qualification; neither virtual tests
nor host data comparison proves complete Linux boot.


## Dell SD firmware transient read recovery — 0.5.50

With Boot 0.1.28 and OFS EFI 0.5.49 on the Dell Latitude 5550,
BlockIo returned `EFI_DEVICE_ERROR` at partition-relative sector
125092067 during the initial loader operation and at 125093772 during
the later fallback read, which reached file byte offset 1310720.
These are distinct firmware transport failures, not established OFS
data corruption. The canonical OFS reader previously compared the
complete files successfully using Linux reads; firmware-specific I/O
remains a separate qualification boundary.

Version 0.5.50 bounds recovery to **one** additional read-only
512-byte `BlockIo.ReadBlocks` attempt after a 1 ms delay, only on
`EFI_DEVICE_ERROR` while the media is present. Other failures stop
immediately; persistent errors preserve the exact failing sector
and final raw firmware status. Canonical OFS checks are unchanged,
the controller is never reset, and no disk writes are performed.
This is a targeted mitigation, not proof of Dell firmware reliability
or successful kernel/initramfs loading.


## Dell OFS EFI read-ahead experiment — 0.5.51

The Dell's Boot 0.1.30 memory-first test remained at 0% on a progress
indicator updated only every five percent. This did not establish that the
initial file read transferred zero bytes; physical qualification is incomplete.

For EFI file reads only, the adapter can fetch up to eight adjacent 512-byte
physical sectors into one aligned, reusable controller buffer. Each OFS block
is still individually checked by the canonical linked-block reader. No
assumption of contiguity is made; a cache miss follows its actual next-block
pointer. Probing and directory reads remain single-sector operations.

Firmware may reject a larger speculative request. For device/size-rejection
errors, readahead is then disabled for that controller and the driver reads
only the needed 512-byte sector using the existing one-retry-on-device-error
policy. Non-retryable media change, media loss and access failures propagate
without fallback. No on-disk writes, alternate OFS parser or Boot-side
filesystem handling is added.

A passed source contract or OVMF virtual boot does not establish improved
throughput or reliable large-file reads on the Dell's SD controller. Nor can
any driver-side progress display interrupt a blocked synchronous firmware
BlockIo operation. Physical testing remains necessary.


## Adaptive EFI bulk-read fallback — 0.5.52

Boot 0.1.32 completed the 17,029,512-byte OFS kernel file read on the Dell
Latitude 5550 in 1,091 seconds. Phase 4 (Linux EFI handoff and potential
86 MB initramfs read) did not visibly complete. Both initramfs archives could
be listed and referenced the expected 7.0.0-38-generic kernel, though they
were not byte-identical after the host initramfs was rebuilt.

Version 0.5.51 permanently disabled speculative eight-sector reads after
one EFI_DEVICE_ERROR from a bulk operation. This policy could reduce all
subsequent filesystem transfers to separate 512-byte firmware requests,
even if smaller batches work reliably. Version 0.5.52 uses per-controller
8 -> 4 -> 2 -> 1 sector batch step-down on repeated speculative
EFI_DEVICE_ERROR returns. A size/protocol rejection directly selects the
single-sector path. Each speculative failure falls back to the *actual*
requested sector and retains the previous single-sector, one-retry policy.
At most three bulk-window-change diagnostics are printed per controller.

No media modifications, allocation policy changes, filesystem parser
replacement or unbounded retries are introduced. Virtual build/OVMF checks
cannot establish real Dell throughput or whether a firmware synchronous
read can still take 30-90 seconds. Physical qualification is outstanding.


## Physical Dell failure of speculative reads and single-block timing — 0.5.53

In the 0.5.52 physical Dell Latitude 5550 test, EFI BlockIo returned
DEVICE_ERROR for attempted 8-, 4-, and 2-sector read-ahead. The 55% kernel
transfer point took 959 seconds versus 573 seconds for 0.5.51, and the
30-60-second pauses continued after the driver had fallen back to the
single-sector path. The 0.5.52 speculative policy demonstrably worsened
startup on this device; no speculative I/O size is currently qualified.

Version 0.5.53 therefore removes the speculative read-ahead and its cache
state entirely. Each canonical OFS block is fetched exactly as requested
through the existing aligned, partition-relative, 512-byte BlockIo transport.
It preserves the bounded single retry on EFI_DEVICE_ERROR, including the
raw EFI status and actual failed sector. Canonical validation, read-only
protocol semantics, and the Linux kernel image/initramfs ownership boundary
are unchanged.

A bounded firmware transport diagnostic now times each *actual* ReadBlocks
call with UEFI GetTime, reporting the first twelve calls that take at least
2 seconds (sector, attempt, elapsed and status). It treats an unavailable or
inconsistent clock as an unavailable measurement, not an I/O failure. It
cannot interrupt a synchronous firmware operation. The two GetTime calls
per block add some measurement cost; this build is for problem isolation,
not a claim of optimized throughput. When no further slow reports are
allowed, the timing calls are suppressed. Virtual CI and QEMU tests cannot
qualify physical Dell latency.

## Page-backed EFI sector-buffer qualification — 23 October 2026

Physical Dell testing read the same high-numbered OFS sectors successfully
under Linux while EFI `BlockIo.ReadBlocks` intermittently returned
`EFI_DEVICE_ERROR`. The UEFI firmware transport, not a corrupt OFS
checksum or the Linux `ofs.ko` module, is the observed failure boundary.

The EFI adapter now obtains its single 512-byte transfer buffer using
`AllocatePages` instead of pool allocation. The transfer address is at
least 4096-byte aligned, respects any stricter power-of-two
`Media.IoAlign` up to 64 KiB, is backed by contiguous UEFI physical pages,
and is released with `FreePages` on probe/stop/error paths. This isolates
a possible Dell DMA-buffer issue without altering the shared OFS parser,
linked-block traversal, 512-byte BlockIo requests, retry bound, on-media
format, or any Linux module. The static contract and OVMF driver-binding
tests guard these properties.

**Status: experimental, physical Dell validation required.** This change
is a targeted transport-compatibility hypothesis, not evidence that the
SD reader's failing firmware calls are fixed. In particular, a firmware
defect with large partition-relative LBAs would not be solved by buffer
alignment alone. Do not publish as a physically qualified driver until
the complete kernel read and handoff have succeeded on the Dell.

## Dell RTS525A firmware DMA-address limit experiment — 0.5.67

Linux `rtsx_pci` configures a 32-bit DMA mask for the RTS525A PCIe SD reader. EFI
`BlockIo.ReadBlocks` failures on the Dell Latitude 5550 were observed even
though Linux could read the same partition-relative sectors. The previous
page-aligned UEFI buffer used `AllocateAnyPages`, potentially placing it above
4 GiB; alignment alone did not enforce the hardware's DMA addressing range.

The OFS UEFI adapter now requests physical pages using `AllocateMaxAddress`
with a maximum address of `0xffffffff` and validates that its complete 512-byte
aligned transfer is below that limit. Failure to allocate suitable memory is
a hard error, not permission to use a potentially unreachable address. The
OFS reader, media data, BlockIo read sizes, retry count and Linux modules are
unchanged. This targets one plausible failure mode; Dell firmware may have
other SD-controller faults, and the correction is **not physically qualified**
until a new on-device trial confirms full kernel and initrd loading.

## Exact Dell RTS525A PCIe ASPM firmware mitigation — 0.5.73

The 0.5.67 physical Dell test proved that 4 KiB aligned/below-4-GiB
BlockIo buffers alone do not solve the intermittent 30-second `EFI_DEVICE_ERROR`
events during OFS kernel loading. Linux diagnostics identified the exact
Realtek RTS525A PCIe endpoint 10ec:525a with Dell subsystem 1028:0cb9,
its Intel 8086:7e39 upstream root port, and ASPM L1 enabled at both ends.

`rtsx525a_link.c` performs one best-effort, volatile firmware-only PCI
configuration change when **all** identifying information matches, before
registering the OFS DriverBinding protocol. It locates the endpoint and
its actual parent by segment/secondary bus, walks the standard PCI
capability chain to the PCIe Link Control register, disables ASPM L0s/L1
on the two link partners with verification and partial-failure rollback.
It does not reset the controller, issue SD/MMC commands, modify PCIe link
speed, change persistent BIOS/NVRAM settings or write the SD card. The
standard firmware BlockIo implementation remains in use; other hardware
is completely untouched. The OS may reconfigure PCIe after UEFI handoff.

**Experimental: no physical Dell pass yet.** A compiler/OVMF success and
source contract cannot determine whether L1 wake transitions cause these
firmware timeouts, nor whether firmware exposes PCI I/O protocols for the
specific devices. The driver prints one status message if the exact
mitigation was applied, or an error if the exact controller was detected
but the operation could not be completed. Test against a backed-up EFI
driver with a working alternative boot entry; do not treat as production
qualification before kernel and initrd load successfully on physical SD.

## Portable storage-provider boundary — 0.5.74

OFS is a **filesystem** driver. Hardware-specific PCIe controller work
does not belong in its `efi_main`, and no OFS probing or reads may
modify controller PCI configuration. Earlier versions 0.5.73 and
0.5.67 physically loaded the Linux kernel but still suffered recurring
30-second `EFI_DEVICE_ERROR` sector read timeouts on the Dell
RTS525A. PCIe ASPM configuration and 32-bit bounce-buffer experiments
are not accepted physical fixes.

0.5.74 removes the Dell-specific `rtsx525a_link` implementation from
the OFS production driver, retaining conservative page-backed DMA
buffers and format-neutral standard UEFI `EFI_BLOCK_IO.ReadBlocks`.
Source of the unsuccessful experiment is retained separately under
`firmware/uefi/storage/experimental/rtsx525a/` for audit, but is not
compiled into OFS or shipped as a native controller driver.

The firmware's 30-second synchronous block-read failures **remain
unresolved**. Boot can now load an independently qualified native
storage provider from `storage_x64/` before normal filesystem drivers
from `drivers_x64/`; the provider must publish standard BlockIo
handles. No qualifying native Realtek RTS525A provider exists yet.

## Bounded DiskIo cache comparison — 0.5.75 (experimental)

The established EFI FSW filesystem implementation caches 128 KiB via
`EFI_DISK_IO.ReadDisk`, whereas the original OFS firmware adapter called
`EFI_BLOCK_IO.ReadBlocks` for every 512-byte block. XFS's Linux source is
present in the repository, but this is **not** a claim that the XFS EFI
boot path uses FSW. A successful XFS boot on the same media undermines a
controller-wide failure diagnosis and prioritizes an OFS-specific comparison.

The new OFS experiment preserves exact 512-byte canonical OFS parser input
and checksum validation. After observing consecutive requested sectors, it
makes one partition-bounded, read-only 128 KiB DiskIo request and caches
successful results. A rejected bulk read disables all future prefetch on
that controller and immediately returns to the original bounded 512-byte
BlockIo path. It does not attempt another speculative size, retry the bulk
operation, write to the card or change platform PCI settings.

The cache is backed by page-aligned physical memory below 4 GiB and is freed
at probe teardown, controller stop or driver startup failure. Neither a
passing EFI build nor virtual OVMF proves performance or compatibility on
the Dell. Do not call this fix physically qualified until a successful OFS
kernel and initrd boot has been observed.
