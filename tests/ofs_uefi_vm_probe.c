/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Isolated OVMF acceptance probe: load the actual Filesystems EFI driver,
 * bind a real GPT partition, and exercise EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.
 * The fixture's nested file contains "HELLO"; this is not a kernel boot test.
 *
 * COM1 stage markers survive a firmware hang. The debug-exit port terminates
 * QEMU deterministically without relying on firmware power-off services.
 */
#include <efi.h>
#include <efilib.h>

#define COM1 0x3f8U
#define EXIT_PORT 0xf4U

static EFI_GUID g_loaded_image = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID g_block_io = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID g_simple_fs = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static void uart(const char *s)
{
    while (*s) {
        unsigned int spins = 0U;
        while ((inb(COM1 + 5U) & 0x20U) == 0U && ++spins < 100000U)
            ;
        outb(COM1, (unsigned char)*s++);
    }
}
static void finish(const char *message, unsigned char code)
{
    uart(message);
    outb(EXIT_PORT, code);
    for (;;)
        __asm__ volatile("pause");
}

/* GNU-EFI crt0 enters efi_main through its C ABI; protocol callbacks are EFIAPI. */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *table)
{
    EFI_STATUS status;
    EFI_LOADED_IMAGE *loaded = NULL;
    EFI_DEVICE_PATH *path;
    EFI_HANDLE driver = NULL;
    EFI_HANDLE *handles = NULL;
    EFI_HANDLE target = NULL;
    UINTN count = 0U, i;
    EFI_BLOCK_IO *block;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
#ifndef OFS_VM_EXPECT_UNSUPPORTED
    EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
    CHAR8 content[6] = {0};
    UINTN length = 5U;
#endif

    InitializeLib(image, table);
    uart("OFS_VM begin\r\n");

    uart("OFS_VM loaded HandleProtocol begin\r\n");
    status = BS->HandleProtocol(image, &g_loaded_image, (VOID **)&loaded);
    uart("OFS_VM loaded HandleProtocol returned\r\n");
    if (EFI_ERROR(status) || !loaded)
        finish("OFS_VM_FAIL loaded image\r\n", 0x11U);

    uart("OFS_VM FileDevicePath begin\r\n");
    path = FileDevicePath(loaded->DeviceHandle, L"\\OFS_x64.efi");
    uart("OFS_VM FileDevicePath returned\r\n");
    if (!path)
        finish("OFS_VM_FAIL driver path\r\n", 0x11U);
    uart("OFS_VM LoadImage begin\r\n");
    status = BS->LoadImage(FALSE, image, path, NULL, 0U, &driver);
    if (EFI_ERROR(status))
        finish("OFS_VM_FAIL LoadImage\r\n", 0x11U);

    uart("OFS_VM StartImage begin\r\n");
    status = BS->StartImage(driver, NULL, NULL);
    if (EFI_ERROR(status))
        finish("OFS_VM_FAIL StartImage\r\n", 0x11U);
    uart("OFS_VM StartImage complete\r\n");

    status = BS->LocateHandleBuffer(ByProtocol, &g_block_io,
                                    NULL, &count, &handles);
    if (EFI_ERROR(status))
        finish("OFS_VM_FAIL enumerate BlockIo\r\n", 0x11U);
    for (i = 0U; i < count; ++i) {
        block = NULL;
        if (EFI_ERROR(BS->HandleProtocol(handles[i], &g_block_io,
                                         (VOID **)&block)) ||
            !block || !block->Media)
            continue;
        if (block->Media->MediaPresent && block->Media->LogicalPartition &&
            block->Media->BlockSize == 512U &&
            block->Media->LastBlock == 1023ULL) {
            target = handles[i];
            break;
        }
    }
    BS->FreePool(handles);
    if (!target)
        finish("OFS_VM_FAIL OFS partition undiscovered\r\n", 0x11U);

    uart("OFS_VM ConnectController begin\r\n");
    status = BS->ConnectController(target, NULL, NULL, FALSE);
    uart("OFS_VM ConnectController returned\r\n");
    fs = NULL;
    status = BS->HandleProtocol(target, &g_simple_fs, (VOID **)&fs);
#ifdef OFS_VM_EXPECT_UNSUPPORTED
    if (!EFI_ERROR(status))
        finish("OFS_VM_FAIL alien media bound\r\n", 0x11U);
    finish("OFS_VM_PASS unsupported media rejected\r\n", 0x10U);
#else
    if (EFI_ERROR(status) || !fs)
        finish("OFS_VM_FAIL no filesystem binding\r\n", 0x11U);

    uart("OFS_VM OpenVolume begin\r\n");
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || !root)
        finish("OFS_VM_FAIL OpenVolume\r\n", 0x11U);
    uart("OFS_VM nested Open begin\r\n");
    status = root->Open(root, &file, L"\\boot\\kernel.efi",
                        EFI_FILE_MODE_READ, 0ULL);
    if (EFI_ERROR(status) || !file)
        finish("OFS_VM_FAIL nested Open\r\n", 0x11U);
    uart("OFS_VM file Read begin\r\n");
    status = file->Read(file, &length, content);
    if (EFI_ERROR(status) || length != 5U ||
        content[0] != 'H' || content[1] != 'E' ||
        content[2] != 'L' || content[3] != 'L' || content[4] != 'O')
        finish("OFS_VM_FAIL file read contents\r\n", 0x11U);
    file->Close(file);
    root->Close(root);

    /* A read-only filesystem still owns protocol lifetime. Disconnect must
     * revoke Simple File System and close DiskIo; reconnection must work. */
    uart("OFS_VM DisconnectController begin\r\n");
    status = BS->DisconnectController(target, driver, NULL);
    if (EFI_ERROR(status))
        finish("OFS_VM_FAIL DisconnectController\r\n", 0x11U);
    fs = NULL;
    status = BS->HandleProtocol(target, &g_simple_fs, (VOID **)&fs);
    if (!EFI_ERROR(status))
        finish("OFS_VM_FAIL filesystem survived Stop\r\n", 0x11U);
    uart("OFS_VM ConnectController again begin\r\n");
    status = BS->ConnectController(target, NULL, NULL, FALSE);
    fs = NULL;
    if (EFI_ERROR(status) ||
        EFI_ERROR(BS->HandleProtocol(target, &g_simple_fs, (VOID **)&fs)) ||
        !fs)
        finish("OFS_VM_FAIL rebind after Stop\r\n", 0x11U);
    finish("OFS_VM_PASS nested read and Stop/rebind\r\n", 0x10U);
#endif
    return EFI_SUCCESS;
}
