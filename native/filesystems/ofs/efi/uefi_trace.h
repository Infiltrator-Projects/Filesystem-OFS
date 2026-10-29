/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_OFS_UEFI_TRACE_H
#define IFS_OFS_UEFI_TRACE_H

/*
 * Temporary opt-in firmware diagnostics. Emit pre/post markers on COM1
 * without relying on GOP, a mounted filesystem, NVRAM or Boot's UI.
 * A marker with no matching completion identifies the blocked firmware call.
 * This code is excluded from normal OFS_x64.efi builds.
 */
#ifdef IFS_OFS_UEFI_TRACE
static inline unsigned char ofs_io_in(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void ofs_io_out(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static void ofs_trace(const char *message)
{
    const char *s = message;
    while (*s) {
        unsigned int spins = 0U;
        while ((ofs_io_in(0x3fdU) & 0x20U) == 0U && spins++ < 100000U)
            ;
        ofs_io_out(0x3f8U, (unsigned char)*s++);
    }
}
static void ofs_trace_status(const char *stage, UINTN code)
{
    static const char hex[] = "0123456789abcdef";
    char bytes[20];
    unsigned int i;
    ofs_trace(stage);
    for (i = 0U; i < 16U; ++i)
        bytes[i] = hex[(code >> (60U - 4U * i)) & 15U];
    bytes[16] = '\r';
    bytes[17] = '\n';
    bytes[18] = 0;
    ofs_trace(bytes);
}
#else
#define ofs_trace(message) ((void)0)
#define ofs_trace_status(stage, code) ((void)0)
#endif
#endif
