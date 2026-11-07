/* SPDX-License-Identifier: GPL-3.0-or-later
 * Compiler lowering of structure copies can require these even when the
 * source uses no libc. Keep them inside the ELF image, including under LTO.
 */
#include <stddef.h>
#if defined(__GNUC__) && !defined(__clang__)
#define IFS_FREESTANDING_MEM __attribute__((used, externally_visible, visibility("hidden")))
#else
#define IFS_FREESTANDING_MEM __attribute__((used, visibility("hidden")))
#endif
IFS_FREESTANDING_MEM
void *memcpy(void *destination, const void *source, size_t bytes) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < bytes; ++i) out[i] = in[i];
    return destination;
}
IFS_FREESTANDING_MEM
void *memset(void *destination, int value, size_t bytes) {
    unsigned char *out = destination;
    for (size_t i = 0; i < bytes; ++i) out[i] = (unsigned char)value;
    return destination;
}
