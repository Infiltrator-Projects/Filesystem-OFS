/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Infiltrator Filesystems: read-only AmigaDOS OFS UEFI driver.
 *
 * Independently built in Filesystems. Infiltrator Boot consumes only standard
 * EFI Driver Binding and EFI Simple File System; it owns none of the OFS code.
 * No persistent write operation is implemented.
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <stdint.h>
#include "reader.h"
#include "uefi_trace.h"

#define OFS_MAX_NAME 30U
#define OFS_SIGNATURE 0x4f465333U
#define OFS_FILE_SIGNATURE 0x4f464633U
#define OFS_MAX_IO_ALIGNMENT (64U * 1024U)
#define OFS_DMA_PAGE_ALIGNMENT 4096U
#define OFS_DMA_MAX_ADDRESS 0xffffffffULL /* Conservative 32-bit firmware DMA window. */
#define OFS_SLOW_READ_REPORT_LIMIT 12U
#define OFS_READ_CACHE_BYTES (128U * 1024U)
#define OFS_READ_CACHE_SECTORS (OFS_READ_CACHE_BYTES / 512U)

typedef struct OfsController OfsController;
typedef struct OfsFile OfsFile;

struct OfsController {
    UINT32 signature;
    EFI_HANDLE handle;
    EFI_DISK_IO *disk;
    EFI_BLOCK_IO *block;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL filesystem;
    IfsOfsEfiVolume volume;
    CHAR16 volume_label[OFS_MAX_NAME + 1U];
    UINTN open_files;
    EFI_STATUS last_read_error; /* Error of the current canonical reader operation. */
    EFI_STATUS last_block_status; /* Unmodified BlockIo error for diagnosis. */
    UINT32 last_failed_sector; /* Partition-relative 512-byte block number. */
    EFI_PHYSICAL_ADDRESS sector_allocation; /* AllocatePages physical base. */
    UINTN sector_allocation_pages; /* Exact count for FreePages. */
    VOID *sector_buffer; /* DMA-safe, suitably aligned sector pointer. */
    UINTN slow_read_reports; /* Bounded, per-controller firmware diagnostics. */
    EFI_PHYSICAL_ADDRESS cache_allocation; /* Separately bounded DiskIo buffer. */
    UINTN cache_allocation_pages;
    VOID *cache_buffer;
    UINT32 cache_first_sector;
    UINT32 cache_media_id; /* Never reuse data after removable media change. */
    UINTN cache_sector_count;
    BOOLEAN cache_valid;
    BOOLEAN cache_disabled; /* One failure permanently disables prefetch. */
    BOOLEAN disk_sector_disabled; /* One failed 512-byte DiskIo selects BlockIo. */
    BOOLEAN have_previous_sector;
    UINT32 previous_sector;
};

struct OfsFile {
    EFI_FILE_PROTOCOL protocol;
    UINT32 signature;
    OfsController *owner;
    IfsOfsEfiEntry entry;
    IfsOfsEfiFileCursor cursor;
    IfsOfsEfiDirCursor dir_cursor;
    UINT64 position;
    CHAR16 name[OFS_MAX_NAME + 1U];
};

static EFI_GUID g_block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID g_disk_guid = EFI_DISK_IO_PROTOCOL_GUID;
static EFI_GUID g_fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID g_binding_guid = EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_GUID g_file_info_guid = EFI_FILE_INFO_ID;
static EFI_GUID g_fs_info_guid = EFI_FILE_SYSTEM_INFO_ID;
static EFI_GUID g_fs_label_guid = EFI_FILE_SYSTEM_VOLUME_LABEL_INFO_ID;

static EFI_STATUS EFIAPI ofs_supported(EFI_DRIVER_BINDING_PROTOCOL *self,
    EFI_HANDLE controller, EFI_DEVICE_PATH *remaining);
static EFI_STATUS EFIAPI ofs_start(EFI_DRIVER_BINDING_PROTOCOL *self,
    EFI_HANDLE controller, EFI_DEVICE_PATH *remaining);
static EFI_STATUS EFIAPI ofs_stop(EFI_DRIVER_BINDING_PROTOCOL *self,
    EFI_HANDLE controller, UINTN child_count, EFI_HANDLE *children);

static EFI_DRIVER_BINDING_PROTOCOL g_binding = {
    ofs_supported, ofs_start, ofs_stop, 0x10U, NULL, NULL
};

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *x = (const UINT8 *)a;
    const UINT8 *y = (const UINT8 *)b;
    UINTN i;
    for (i = 0U; i < sizeof(EFI_GUID); ++i)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static EFI_STATUS allocate_zero(UINTN bytes, VOID **out)
{
    EFI_STATUS result;
    if (!out || bytes == 0U)
        return EFI_INVALID_PARAMETER;
    *out = NULL;
    result = BS->AllocatePool(EfiBootServicesData, bytes, out);
    if (EFI_ERROR(result))
        return result;
    BS->SetMem(*out, bytes, 0U);
    return EFI_SUCCESS;
}

/* The freestanding OFS reader returns only format-neutral -1/0/1 results.
 * Preserve firmware transport failures separately from metadata rejection.
 * One repeat of a transient EFI_DEVICE_ERROR is permitted; all reads remain
 * read-only, bounded, and subject to canonical block validation. */
static EFI_STATUS reader_error(const OfsController *ctx)
{
    return ctx && EFI_ERROR(ctx->last_read_error)
        ? ctx->last_read_error : EFI_VOLUME_CORRUPTED;
}

/* The portable OFS reader uses 512-byte stack arrays, which do not meet
 * arbitrary firmware DMA alignment requirements. Keep exactly one bounded,
 * aligned sector buffer per controller. Supported() releases its probe buffer. */
/* Some firmware-backed removable-media controllers reject unaligned DMA
 * buffers even when Media.IoAlign understates their actual requirements.
 * Allocate whole UEFI physical pages, never a transient pool allocation;
 * honour any stricter advertised IoAlign and keep a full 512-byte transfer
 * within the allocation. Limit this memory to the low 4 GiB for broad
 * compatibility with UEFI controllers lacking 64-bit DMA access; never
 * assume that the filesystem driver knows which controller is beneath
 * BlockIo. Fail closed if suitable memory is unavailable. OFS data
 * validation is unchanged. */
static EFI_STATUS ensure_sector_buffer(OfsController *ctx)
{
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS address = OFS_DMA_MAX_ADDRESS;
    UINTN alignment, pages, aligned;
    if (ctx->sector_buffer)
        return EFI_SUCCESS;
    alignment = (UINTN)ctx->block->Media->IoAlign;
    if (alignment == 0U)
        alignment = 1U;
    if ((alignment & (alignment - 1U)) != 0U ||
        alignment > OFS_MAX_IO_ALIGNMENT)
        return EFI_UNSUPPORTED;
    if (alignment < OFS_DMA_PAGE_ALIGNMENT)
        alignment = OFS_DMA_PAGE_ALIGNMENT;
    pages = (512U + alignment - 1U + OFS_DMA_PAGE_ALIGNMENT - 1U) /
            OFS_DMA_PAGE_ALIGNMENT;
    status = BS->AllocatePages(AllocateMaxAddress, EfiBootServicesData,
                               pages, &address);
    if (EFI_ERROR(status))
        return status;
    aligned = ((UINTN)address + alignment - 1U) & ~(alignment - 1U);
    if ((UINT64)aligned + 511ULL > OFS_DMA_MAX_ADDRESS ||
        aligned < (UINTN)address) {
        BS->FreePages(address, pages);
        return EFI_UNSUPPORTED;
    }
    ctx->sector_allocation = address;
    ctx->sector_allocation_pages = pages;
    ctx->sector_buffer = (VOID *)aligned;
    return EFI_SUCCESS;
}

/* The existing EFI filesystem framework uses a 128 KiB DiskIo read cache
 * rather than sending every file-data sector separately through BlockIo.
 * OFS keeps its canonical 512-byte parser but can opportunistically prefetch
 * ONLY after two consecutive successful sector requests. A single rejected
 * DiskIo prefetch disables this path for the controller and falls back to the
 * previously qualified one-sector BlockIo path; never retry bulk reads. */
static EFI_STATUS ensure_read_cache(OfsController *ctx)
{
    EFI_PHYSICAL_ADDRESS address = OFS_DMA_MAX_ADDRESS;
    EFI_STATUS status;
    UINTN alignment, pages, aligned;
    if (ctx->cache_buffer)
        return EFI_SUCCESS;
    alignment = (UINTN)ctx->block->Media->IoAlign;
    if (alignment == 0U)
        alignment = 1U;
    if ((alignment & (alignment - 1U)) != 0U ||
        alignment > OFS_MAX_IO_ALIGNMENT)
        return EFI_UNSUPPORTED;
    if (alignment < OFS_DMA_PAGE_ALIGNMENT)
        alignment = OFS_DMA_PAGE_ALIGNMENT;
    pages = (OFS_READ_CACHE_BYTES + alignment - 1U +
             OFS_DMA_PAGE_ALIGNMENT - 1U) / OFS_DMA_PAGE_ALIGNMENT;
    status = BS->AllocatePages(AllocateMaxAddress, EfiBootServicesData,
                               pages, &address);
    if (EFI_ERROR(status))
        return status;
    aligned = ((UINTN)address + alignment - 1U) & ~(alignment - 1U);
    if (aligned < (UINTN)address ||
        (UINT64)aligned + OFS_READ_CACHE_BYTES - 1ULL >
        OFS_DMA_MAX_ADDRESS) {
        BS->FreePages(address, pages);
        return EFI_UNSUPPORTED;
    }
    ctx->cache_allocation = address;
    ctx->cache_allocation_pages = pages;
    ctx->cache_buffer = (VOID *)aligned;
    return EFI_SUCCESS;
}

/* 1 = sector supplied from a verified in-memory DiskIo cache, 0 = fallback
 * to existing single-sector BlockIo. Cache I/O is strictly read-only, inside
 * the partition, and never suppresses canonical OFS checksum validation. */
static int try_cached_sector(OfsController *ctx, UINT32 number, uint8_t data[512])
{
    EFI_STATUS status;
    UINT64 available;
    UINTN blocks;
    if (ctx->cache_valid &&
        ctx->cache_media_id == ctx->block->Media->MediaId &&
        number >= ctx->cache_first_sector &&
        (UINT64)number < (UINT64)ctx->cache_first_sector +
                         (UINT64)ctx->cache_sector_count) {
        UINTN offset = (UINTN)(number - ctx->cache_first_sector) * 512U;
        BS->CopyMem(data, (UINT8 *)ctx->cache_buffer + offset, 512U);
        return 1;
    }
    /* A changed MediaId invalidates all prior cached partition data. */
    if (ctx->cache_valid &&
        ctx->cache_media_id != ctx->block->Media->MediaId)
        ctx->cache_valid = FALSE;
    if (ctx->cache_disabled || ctx->disk_sector_disabled || !ctx->disk ||
        !ctx->have_previous_sector ||
        (UINT64)ctx->previous_sector + 1ULL != (UINT64)number)
        return 0;
    status = ensure_read_cache(ctx);
    if (EFI_ERROR(status)) {
        ctx->cache_disabled = TRUE;
        return 0;
    }
    available = ctx->block->Media->LastBlock - (UINT64)number + 1ULL;
    blocks = available > OFS_READ_CACHE_SECTORS
        ? OFS_READ_CACHE_SECTORS : (UINTN)available;
    if (blocks < 2U)
        return 0;
    /* DiskIo addresses bytes, unlike partition-relative BlockIo sectors. */
    status = ctx->disk->ReadDisk(ctx->disk, ctx->block->Media->MediaId,
                                 (UINT64)number * 512ULL,
                                 blocks * 512U, ctx->cache_buffer);
    if (EFI_ERROR(status)) {
        /* Dell firmware rejected earlier speculative multi-sector BlockIo.
         * Disable this independent DiskIo experiment after ONE failure. */
        ctx->cache_disabled = TRUE;
        ctx->cache_valid = FALSE;
        Print(L"OFS EFI: DiskIo cache rejected at sector %u: %r; single-sector fallback\n",
              number, status);
        return 0;
    }
    ctx->cache_first_sector = number;
    ctx->cache_media_id = ctx->block->Media->MediaId;
    ctx->cache_sector_count = blocks;
    ctx->cache_valid = TRUE;
    BS->CopyMem(data, ctx->cache_buffer, 512U);
    return 1;
}

static VOID release_sector_buffer(OfsController *ctx)
{
    if (ctx && ctx->sector_allocation_pages != 0U) {
        BS->FreePages(ctx->sector_allocation, ctx->sector_allocation_pages);
        ctx->sector_allocation = 0U;
        ctx->sector_allocation_pages = 0U;
        ctx->sector_buffer = NULL;
    }
    if (ctx && ctx->cache_allocation_pages != 0U) {
        BS->FreePages(ctx->cache_allocation, ctx->cache_allocation_pages);
        ctx->cache_allocation = 0U;
        ctx->cache_allocation_pages = 0U;
        ctx->cache_buffer = NULL;
        ctx->cache_valid = FALSE;
    }
}

/* Only an actual OFS block is requested. Physical Dell tests showed
 * speculative 8-, 4- and 2-sector requests fail with EFI_DEVICE_ERROR,
 * adding repeated 30-second delays before their single-sector fallbacks.
 * Observations are bounded to the first twelve slow firmware calls so an
 * unusable controller cannot flood the pre-boot console indefinitely.
 * RTC timing is diagnostic, not a deadline: ReadBlocks is synchronous. */
static EFI_STATUS read_one_block(OfsController *ctx, UINT32 number, UINTN attempt)
{
    EFI_TIME before, after;
    EFI_STATUS status;
    BOOLEAN timed = FALSE;
    UINTN start, end, elapsed;

    if (ctx->slow_read_reports < OFS_SLOW_READ_REPORT_LIMIT)
        timed = !EFI_ERROR(RT->GetTime(&before, NULL));
    /* Use the byte-addressed DiskIo protocol used by the established EFI
     * filesystem framework before sequential cache detection. A rejected
     * single-sector DiskIo operation is not repeated; preserve the original
     * BlockIo path as a fallback without altering OFS validation. */
    if (ctx->disk && !ctx->disk_sector_disabled) {
        ofs_trace("OFS ReadDisk sector begin\r\n");
        status = ctx->disk->ReadDisk(ctx->disk, ctx->block->Media->MediaId,
                                     (UINT64)number * 512ULL, 512U,
                                     ctx->sector_buffer);
        if (EFI_ERROR(status) && status != EFI_NO_MEDIA &&
            status != EFI_MEDIA_CHANGED) {
            ctx->disk_sector_disabled = TRUE;
            ctx->cache_disabled = TRUE;
            Print(L"OFS EFI: single-sector DiskIo rejected at %u: %r; using BlockIo\n",
                  number, status);
            status = ctx->block->ReadBlocks(ctx->block,
                        ctx->block->Media->MediaId, (EFI_LBA)number, 512U,
                        ctx->sector_buffer);
        }
    } else {
        ofs_trace("OFS ReadBlocks begin\r\n");
        status = ctx->block->ReadBlocks(ctx->block, ctx->block->Media->MediaId,
                                        (EFI_LBA)number, 512U,
                                        ctx->sector_buffer);
    }
    ofs_trace(EFI_ERROR(status) ? "OFS sector read error\r\n" :
                                  "OFS sector read complete\r\n");
    if (!timed || EFI_ERROR(RT->GetTime(&after, NULL)))
        return status;
    start = (UINTN)before.Hour * 3600U + (UINTN)before.Minute * 60U +
            (UINTN)before.Second;
    end = (UINTN)after.Hour * 3600U + (UINTN)after.Minute * 60U +
          (UINTN)after.Second;
    if (end < start)
        end += 86400U;
    elapsed = end - start;
    if (elapsed >= 2U && elapsed <= 3600U) {
        ++ctx->slow_read_reports;
        Print(L"OFS EFI: sector %u, attempt %u: %u sec, %r\n",
              number, attempt + 1U, elapsed, status);
    }
    return status;
}

static int read_sector(void *opaque, uint32_t number, uint8_t data[512])
{
    OfsController *ctx = (OfsController *)opaque;
    EFI_STATUS result;
    UINTN attempt;
    if (!ctx)
        return -1;
    if (!ctx->block || !ctx->block->Media || !data) {
        ctx->last_read_error = EFI_DEVICE_ERROR;
        return -1;
    }
    if (!ctx->block->Media->MediaPresent) {
        ctx->last_read_error = EFI_NO_MEDIA;
        return -1;
    }
    if ((UINT64)number > ctx->block->Media->LastBlock) {
        ctx->last_read_error = EFI_VOLUME_CORRUPTED;
        return -1;
    }
    result = ensure_sector_buffer(ctx);
    if (EFI_ERROR(result)) {
        ctx->last_read_error = result;
        return -1;
    }
    if (try_cached_sector(ctx, number, data)) {
        ctx->previous_sector = number;
        ctx->have_previous_sector = TRUE;
        return 0;
    }
    /* Keep retries limited to one actual 512-byte block request.
     * A failed cache-prefetch never changes the ordinary BlockIo fallback. */
    for (attempt = 0U; attempt < 2U; ++attempt) {
        result = read_one_block(ctx, number, attempt);
        if (result != EFI_DEVICE_ERROR ||
            !ctx->block->Media->MediaPresent || attempt != 0U)
            break;
        ofs_trace("OFS ReadBlocks device error; retrying once\r\n");
        BS->Stall(1000U);
    }
    if (EFI_ERROR(result)) {
        ctx->last_failed_sector = number;
        ctx->last_block_status = result;
        ctx->last_read_error = result == EFI_VOLUME_CORRUPTED
            ? EFI_DEVICE_ERROR : result;
        return -1;
    }
    BS->CopyMem(data, ctx->sector_buffer, 512U);
    ctx->previous_sector = number;
    ctx->have_previous_sector = TRUE;
    return 0;
}

/* Probe at most the boot block and midpoint root; reject alien media quickly. */
static EFI_STATUS ofs_probe(OfsController *ctx, EFI_HANDLE controller)
{
    EFI_STATUS status;
    VOID *existing = NULL;
    EFI_BLOCK_IO *block = NULL;
    EFI_DISK_IO *disk = NULL;

    if (!ctx)
        return EFI_INVALID_PARAMETER;
    status = BS->HandleProtocol(controller, &g_fs_guid, &existing);
    if (!EFI_ERROR(status))
        return EFI_ALREADY_STARTED;
    status = BS->HandleProtocol(controller, &g_block_guid, (VOID **)&block);
    if (EFI_ERROR(status) || !block || !block->Media)
        return EFI_UNSUPPORTED;
    /* Only partition-relative DOS/0 is supported; a whole-disk handle must
     * first be interpreted by a partition-map adapter (e.g. Amiga RDB). */
    if (!block->Media->MediaPresent || !block->Media->LogicalPartition ||
        block->Media->BlockSize != 512U ||
        block->Media->LastBlock < 15ULL ||
        block->Media->LastBlock >= 0xffffffffULL)
        return EFI_UNSUPPORTED;
    status = BS->HandleProtocol(controller, &g_disk_guid, (VOID **)&disk);
    if (EFI_ERROR(status) || !disk)
        return EFI_UNSUPPORTED;
    ctx->handle = controller;
    ctx->disk = disk;
    ctx->block = block;
    if (ifs_ofs_efi_mount(&ctx->volume,
                          (uint32_t)(block->Media->LastBlock + 1ULL),
                          read_sector, ctx) != 0)
        return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ofs_supported(EFI_DRIVER_BINDING_PROTOCOL *self,
    EFI_HANDLE controller, EFI_DEVICE_PATH *remaining)
{
    OfsController probe;
    (void)self; (void)remaining;
    EFI_STATUS status;
    ofs_trace("OFS Supported begin\r\n");
    BS->SetMem(&probe, sizeof(probe), 0U);
    status = ofs_probe(&probe, controller);
    release_sector_buffer(&probe);
    ofs_trace(EFI_ERROR(status) ? "OFS Supported rejected\r\n" :
                                  "OFS Supported accepted\r\n");
    return status;
}

/* Operations below expose EFI read-only file handles. */
static EFI_STATUS EFIAPI ofs_open(EFI_FILE_PROTOCOL *file,
    EFI_FILE_PROTOCOL **out, CHAR16 *filename, UINT64 mode, UINT64 attributes);
static EFI_STATUS EFIAPI ofs_close(EFI_FILE_PROTOCOL *file);
static EFI_STATUS EFIAPI ofs_delete(EFI_FILE_PROTOCOL *file);
static EFI_STATUS EFIAPI ofs_read(EFI_FILE_PROTOCOL *file, UINTN *length, VOID *buffer);
static EFI_STATUS EFIAPI ofs_write(EFI_FILE_PROTOCOL *file, UINTN *length, VOID *buffer);
static EFI_STATUS EFIAPI ofs_get_position(EFI_FILE_PROTOCOL *file, UINT64 *position);
static EFI_STATUS EFIAPI ofs_set_position(EFI_FILE_PROTOCOL *file, UINT64 position);
static EFI_STATUS EFIAPI ofs_get_info(EFI_FILE_PROTOCOL *file, EFI_GUID *kind,
    UINTN *length, VOID *buffer);
static EFI_STATUS EFIAPI ofs_set_info(EFI_FILE_PROTOCOL *file, EFI_GUID *kind,
    UINTN length, VOID *buffer);
static EFI_STATUS EFIAPI ofs_flush(EFI_FILE_PROTOCOL *file);

static EFI_STATUS make_handle(OfsController *ctx, const IfsOfsEfiEntry *entry,
    const CHAR16 *name, EFI_FILE_PROTOCOL **out)
{
    OfsFile *node;
    UINTN i = 0U;
    EFI_STATUS status;
    if (!ctx || !entry || !out)
        return EFI_INVALID_PARAMETER;
    status = allocate_zero(sizeof(*node), (VOID **)&node);
    if (EFI_ERROR(status))
        return status;
    node->signature = OFS_FILE_SIGNATURE;
    node->owner = ctx;
    node->entry = *entry;
    if (name) {
        while (name[i] != 0U && i < OFS_MAX_NAME) {
            node->name[i] = name[i];
            ++i;
        }
    }
    node->name[i] = 0U;
    node->protocol.Revision = EFI_FILE_PROTOCOL_REVISION;
    node->protocol.Open = ofs_open;
    node->protocol.Close = ofs_close;
    node->protocol.Delete = ofs_delete;
    node->protocol.Read = ofs_read;
    node->protocol.Write = ofs_write;
    node->protocol.GetPosition = ofs_get_position;
    node->protocol.SetPosition = ofs_set_position;
    node->protocol.GetInfo = ofs_get_info;
    node->protocol.SetInfo = ofs_set_info;
    node->protocol.Flush = ofs_flush;
    ++ctx->open_files;
    *out = &node->protocol;
    return EFI_SUCCESS;
}

static OfsFile *file_object(EFI_FILE_PROTOCOL *handle)
{
    OfsFile *node = (OfsFile *)handle;
    if (!node || node->signature != OFS_FILE_SIGNATURE || !node->owner ||
        node->owner->signature != OFS_SIGNATURE)
        return NULL;
    return node;
}

static EFI_STATUS EFIAPI ofs_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,
                                         EFI_FILE_PROTOCOL **root)
{
    OfsController *ctx = (OfsController *)((char *)fs -
                              offsetof(OfsController, filesystem));
    IfsOfsEfiEntry entry;
    if (!root || !ctx || ctx->signature != OFS_SIGNATURE)
        return EFI_INVALID_PARAMETER;
    entry = ifs_ofs_efi_root(&ctx->volume);
    return make_handle(ctx, &entry, L"", root);
}

static EFI_STATUS EFIAPI ofs_open(EFI_FILE_PROTOCOL *file,
    EFI_FILE_PROTOCOL **out, CHAR16 *filename, UINT64 mode, UINT64 attributes)
{
    OfsFile *parent = file_object(file);
    IfsOfsEfiEntry current, candidate;
    CHAR16 segment[OFS_MAX_NAME + 1U];
    uint8_t name[OFS_MAX_NAME];
    UINTN i, n;
    int found;
    (void)attributes;
    if (!parent || !out || !filename)
        return EFI_INVALID_PARAMETER;
    *out = NULL;
    if (mode != EFI_FILE_MODE_READ)
        return EFI_WRITE_PROTECTED;

    current = parent->entry;
    if (*filename == L'\\' || *filename == L'/')
        current = ifs_ofs_efi_root(&parent->owner->volume);
    i = 0U;
    while (filename[i] != 0U) {
        while (filename[i] == L'\\' || filename[i] == L'/')
            ++i;
        if (filename[i] == 0U)
            break;
        if (current.type != IFS_OFS_EFI_DIRECTORY)
            return EFI_NOT_FOUND;
        n = 0U;
        while (filename[i] && filename[i] != L'\\' && filename[i] != L'/') {
            if (n >= OFS_MAX_NAME || filename[i] > 255U)
                return EFI_UNSUPPORTED;
            segment[n] = filename[i];
            name[n] = (uint8_t)filename[i];
            ++i;
            ++n;
        }
        segment[n] = 0U;
        if (n == 1U && segment[0] == L'.')
            continue;
        if (n == 2U && segment[0] == L'.' && segment[1] == L'.')
            return EFI_UNSUPPORTED; /* Never fabricate parent semantics. */
        parent->owner->last_read_error = EFI_SUCCESS;
        found = ifs_ofs_efi_lookup(&parent->owner->volume, &current,
                                    name, n, &candidate);
        if (found != 0)
            return found == 1 ? EFI_NOT_FOUND : reader_error(parent->owner);
        current = candidate;
    }
    /* Extract the final name only for EFI_FILE_INFO; empty means root. */
    n = 0U;
    if (i > 0U) {
        UINTN end = i;
        while (end && (filename[end - 1U] == L'\\' ||
                       filename[end - 1U] == L'/'))
            --end;
        while (end && filename[end - 1U] != L'\\' &&
               filename[end - 1U] != L'/')
            --end;
        while (filename[end] != 0U && filename[end] != L'\\' &&
               filename[end] != L'/' && n < OFS_MAX_NAME)
            segment[n++] = filename[end++];
    }
    segment[n] = 0U;
    return make_handle(parent->owner, &current, segment, out);
}

static EFI_STATUS EFIAPI ofs_close(EFI_FILE_PROTOCOL *file)
{
    OfsFile *node = file_object(file);
    if (!node)
        return EFI_INVALID_PARAMETER;
    if (node->owner->open_files == 0U)
        return EFI_DEVICE_ERROR;
    --node->owner->open_files;
    node->signature = 0U;
    BS->FreePool(node);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ofs_delete(EFI_FILE_PROTOCOL *file)
{
    EFI_STATUS status = ofs_close(file);
    return EFI_ERROR(status) ? status : EFI_WARN_DELETE_FAILURE;
}

static UINTN utf16_length(const CHAR16 *s)
{
    UINTN n = 0U;
    while (s[n])
        ++n;
    return n;
}

/* The EFI_FILE_INFO variable tail includes a terminating UTF-16 NUL. */
static EFI_STATUS write_file_info(const IfsOfsEfiEntry *entry,
    const CHAR16 *name, UINTN *capacity, VOID *buffer)
{
    UINTN n, need, i;
    EFI_FILE_INFO *info;
    if (!entry || !capacity)
        return EFI_INVALID_PARAMETER;
    n = utf16_length(name);
    need = SIZE_OF_EFI_FILE_INFO + (n + 1U) * sizeof(CHAR16);
    if (*capacity < need) {
        *capacity = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (!buffer)
        return EFI_INVALID_PARAMETER;
    BS->SetMem(buffer, need, 0U);
    info = (EFI_FILE_INFO *)buffer;
    info->Size = need;
    info->FileSize = (UINT64)entry->size;
    info->PhysicalSize = entry->type == IFS_OFS_EFI_DIRECTORY
        ? 512ULL : (((UINT64)entry->size + 487ULL) / 488ULL) * 512ULL;
    info->Attribute = EFI_FILE_READ_ONLY |
       (entry->type == IFS_OFS_EFI_DIRECTORY ? EFI_FILE_DIRECTORY : 0U);
    for (i = 0; i < n; ++i)
        info->FileName[i] = name[i];
    info->FileName[n] = 0U;
    *capacity = need;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ofs_read(EFI_FILE_PROTOCOL *file, UINTN *length, VOID *buffer)
{
    OfsFile *node = file_object(file);
    EFI_STATUS status;
    UINTN count;
    int result;
    if (!node || !length || (*length && !buffer))
        return EFI_INVALID_PARAMETER;
    if (node->entry.type == IFS_OFS_EFI_DIRECTORY) {
        IfsOfsEfiEntry entry;
        IfsOfsEfiDirCursor next = node->dir_cursor;
        uint8_t bytes[31];
        CHAR16 name[OFS_MAX_NAME + 1U];
        UINTN i;
        node->owner->last_read_error = EFI_SUCCESS;
        result = ifs_ofs_efi_next(&node->owner->volume, &node->entry,
                                  &next, &entry, bytes);
        if (result == 1) {
            *length = 0U;
            return EFI_SUCCESS;
        }
        if (result != 0)
            return reader_error(node->owner);
        for (i = 0U; i < OFS_MAX_NAME && bytes[i]; ++i)
            name[i] = (CHAR16)bytes[i];
        name[i] = 0U;
        status = write_file_info(&entry, name, length, buffer);
        if (!EFI_ERROR(status))
            node->dir_cursor = next;
        return status;
    }
    if (node->entry.type != IFS_OFS_EFI_FILE)
        return EFI_UNSUPPORTED;
    if (node->position >= node->entry.size) {
        *length = 0U;
        return EFI_SUCCESS;
    }
    count = *length;
    if (count > (UINTN)((UINT64)node->entry.size - node->position))
        count = (UINTN)((UINT64)node->entry.size - node->position);
    if (count == 0U) {
        *length = 0U;
        return EFI_SUCCESS;
    }
    node->owner->last_read_error = EFI_SUCCESS;
    node->owner->last_block_status = EFI_SUCCESS;
    result = ifs_ofs_efi_read(&node->owner->volume, &node->entry,
              (uint32_t)node->position, buffer, count, &node->cursor);
    if (result != 0) {
        *length = 0U;
        status = reader_error(node->owner);
        if (EFI_ERROR(node->owner->last_block_status))
            Print(L"OFS EFI: firmware block read failed at sector %u: %r\n",
                  node->owner->last_failed_sector,
                  node->owner->last_block_status);
        return status;
    }
    node->position += count;
    *length = count;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ofs_write(EFI_FILE_PROTOCOL *file, UINTN *length, VOID *buffer)
{
    (void)file; (void)length; (void)buffer;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI ofs_get_position(EFI_FILE_PROTOCOL *file, UINT64 *position)
{
    OfsFile *node = file_object(file);
    if (!node || !position)
        return EFI_INVALID_PARAMETER;
    if (node->entry.type == IFS_OFS_EFI_DIRECTORY)
        return EFI_UNSUPPORTED;
    *position = node->position;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ofs_set_position(EFI_FILE_PROTOCOL *file, UINT64 position)
{
    OfsFile *node = file_object(file);
    if (!node)
        return EFI_INVALID_PARAMETER;
    if (node->entry.type == IFS_OFS_EFI_DIRECTORY) {
        if (position != 0U)
            return EFI_UNSUPPORTED;
        BS->SetMem(&node->dir_cursor, sizeof(node->dir_cursor), 0U);
        return EFI_SUCCESS;
    }
    if (position == ~((UINT64)0U))
        position = node->entry.size;
    if (position > node->entry.size)
        return EFI_INVALID_PARAMETER;
    node->position = position;
    BS->SetMem(&node->cursor, sizeof(node->cursor), 0U);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ofs_get_info(EFI_FILE_PROTOCOL *file, EFI_GUID *kind,
    UINTN *length, VOID *buffer)
{
    OfsFile *node = file_object(file);
    UINTN label_chars, needed, i;
    if (!node || !kind || !length)
        return EFI_INVALID_PARAMETER;
    if (guid_equal(kind, &g_file_info_guid))
        return write_file_info(&node->entry, node->name, length, buffer);
    if (guid_equal(kind, &g_fs_info_guid)) {
        EFI_FILE_SYSTEM_INFO *info;
        label_chars = utf16_length(node->owner->volume_label);
        needed = SIZE_OF_EFI_FILE_SYSTEM_INFO + (label_chars + 1U) * sizeof(CHAR16);
        if (*length < needed) {
            *length = needed;
            return EFI_BUFFER_TOO_SMALL;
        }
        if (!buffer)
            return EFI_INVALID_PARAMETER;
        BS->SetMem(buffer, needed, 0U);
        info = (EFI_FILE_SYSTEM_INFO *)buffer;
        info->Size = needed;
        info->ReadOnly = TRUE;
        info->VolumeSize = (UINT64)node->owner->volume.blocks * 512ULL;
        info->FreeSpace = 0ULL; /* Unknown: do not claim writable capacity. */
        info->BlockSize = 512U;
        for (i = 0U; i < label_chars; ++i)
            info->VolumeLabel[i] = node->owner->volume_label[i];
        info->VolumeLabel[label_chars] = 0U;
        *length = needed;
        return EFI_SUCCESS;
    }
    if (guid_equal(kind, &g_fs_label_guid)) {
        label_chars = utf16_length(node->owner->volume_label);
        needed = (label_chars + 1U) * sizeof(CHAR16);
        if (*length < needed) {
            *length = needed;
            return EFI_BUFFER_TOO_SMALL;
        }
        if (!buffer)
            return EFI_INVALID_PARAMETER;
        for (i = 0U; i < label_chars; ++i)
            ((CHAR16 *)buffer)[i] = node->owner->volume_label[i];
        ((CHAR16 *)buffer)[label_chars] = 0U;
        *length = needed;
        return EFI_SUCCESS;
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI ofs_set_info(EFI_FILE_PROTOCOL *file, EFI_GUID *kind,
    UINTN length, VOID *buffer)
{
    (void)file; (void)kind; (void)length; (void)buffer;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI ofs_flush(EFI_FILE_PROTOCOL *file)
{
    return file_object(file) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI ofs_start(EFI_DRIVER_BINDING_PROTOCOL *self,
    EFI_HANDLE controller, EFI_DEVICE_PATH *remaining)
{
    EFI_STATUS status;
    OfsController *ctx = NULL;
    EFI_DISK_IO *claimed_disk = NULL;
    uint8_t root[512];
    UINTN i, count;
    (void)self; (void)remaining;
    ofs_trace("OFS Start begin\r\n");
    status = allocate_zero(sizeof(*ctx), (VOID **)&ctx);
    if (EFI_ERROR(status))
        return status;
    status = ofs_probe(ctx, controller);
    if (EFI_ERROR(status)) {
        release_sector_buffer(ctx);
        BS->FreePool(ctx);
        return status;
    }
    /* PartitionDxe or other bus drivers may own BlockIo already. A
     * filesystem driver instead claims DiskIo BY_DRIVER, matching the UEFI
     * FAT driver model. BlockIo is observed without exclusive ownership.
     * Stop must release exactly this DiskIo claim. */
    ofs_trace("OFS OpenProtocol DiskIo begin\r\n");
    status = BS->OpenProtocol(controller, &g_disk_guid,
                              (VOID **)&claimed_disk,
                              self->DriverBindingHandle, controller,
                              EFI_OPEN_PROTOCOL_BY_DRIVER);
    ofs_trace_status("OFS OpenProtocol DiskIo status 0x", status);
    if (EFI_ERROR(status)) {
        release_sector_buffer(ctx);
        BS->FreePool(ctx);
        return status;
    }
    ctx->disk = claimed_disk;
    ctx->signature = OFS_SIGNATURE;
    ctx->filesystem.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    ctx->filesystem.OpenVolume = ofs_open_volume;
    ctx->volume_label[0] = L'O';
    ctx->volume_label[1] = L'F';
    ctx->volume_label[2] = L'S';
    ctx->volume_label[3] = 0U;
    if (read_sector(ctx, ctx->volume.root, root) == 0) {
        count = root[432];
        if (count != 0U && count <= OFS_MAX_NAME) {
            for (i = 0U; i < count; ++i)
                ctx->volume_label[i] = root[433U + i];
            ctx->volume_label[count] = 0U;
        }
    }
    ofs_trace("OFS filesystem protocol install begin\r\n");
    status = BS->InstallProtocolInterface(&controller, &g_fs_guid,
                                          EFI_NATIVE_INTERFACE, &ctx->filesystem);
    if (EFI_ERROR(status)) {
        ctx->signature = 0U;
        BS->CloseProtocol(controller, &g_disk_guid,
                          self->DriverBindingHandle, controller);
        release_sector_buffer(ctx);
        BS->FreePool(ctx);
    }
    ofs_trace(EFI_ERROR(status) ? "OFS Start error\r\n" :
                                  "OFS Start complete\r\n");
    return status;
}

static EFI_STATUS EFIAPI ofs_stop(EFI_DRIVER_BINDING_PROTOCOL *self,
    EFI_HANDLE controller, UINTN child_count, EFI_HANDLE *children)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    OfsController *ctx;
    EFI_STATUS status;
    (void)self; (void)child_count; (void)children;
    status = BS->HandleProtocol(controller, &g_fs_guid, (VOID **)&fs);
    if (EFI_ERROR(status) || !fs)
        return EFI_NOT_STARTED;
    ctx = (OfsController *)((char *)fs - offsetof(OfsController, filesystem));
    if (ctx->signature != OFS_SIGNATURE || ctx->handle != controller)
        return EFI_UNSUPPORTED;
    if (ctx->open_files != 0U)
        return EFI_ACCESS_DENIED;
    ofs_trace("OFS Stop begin\r\n");
    status = BS->UninstallProtocolInterface(controller, &g_fs_guid, fs);
    if (!EFI_ERROR(status)) {
        EFI_STATUS close_status = BS->CloseProtocol(controller, &g_disk_guid,
                                   self->DriverBindingHandle, controller);
        ctx->signature = 0U;
        release_sector_buffer(ctx);
        BS->FreePool(ctx);
        if (EFI_ERROR(close_status))
            return close_status;
    }
    return status;
}

/* GNU-EFI crt0 calls efi_main using the C ABI. Only exported UEFI
 * protocol callbacks use EFIAPI (Microsoft x64); combining both ABIs at
 * this entry point corrupts the image/table arguments on real firmware. */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *table)
{
    EFI_STATUS status;
    InitializeLib(image, table);
    g_binding.ImageHandle = image;
    g_binding.DriverBindingHandle = image;
    ofs_trace("OFS DriverBinding install begin\r\n");
    status = BS->InstallProtocolInterface(&image, &g_binding_guid,
                                          EFI_NATIVE_INTERFACE, &g_binding);
    return status;
}
