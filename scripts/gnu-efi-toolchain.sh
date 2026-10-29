# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by standalone EFI builds; an explicit prefix supports isolated SDKs.
gnu_efi_prefix="${GNU_EFI_PREFIX:-/usr}"
gnu_efi_include="$gnu_efi_prefix/include/efi"
if [[ -n ${GNU_EFI_PREFIX:-} ]]; then
    gnu_efi_files="$(find "$gnu_efi_prefix/lib" -type f -print)"
else
    gnu_efi_files="$(dpkg -L gnu-efi)"
fi
crt="$(grep '/crt0-efi-x86_64\.o$' <<< "$gnu_efi_files" | head -n1)"
ldscript="$(grep '/elf_x86_64_efi\.lds$' <<< "$gnu_efi_files" | head -n1)"
libefi="$(grep '/libefi\.a$' <<< "$gnu_efi_files" | head -n1)"
libgnuefi="$(grep '/libgnuefi\.a$' <<< "$gnu_efi_files" | head -n1)"
for item in "$crt" "$ldscript" "$libefi" "$libgnuefi"; do test -s "$item"; done
test -s "$gnu_efi_include/efi.h"
