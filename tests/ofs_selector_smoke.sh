#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Only run on disposable GitHub-hosted CI runners; does not touch block devices.
set -euo pipefail

if [[ "${GITHUB_ACTIONS:-}" != true ||
      "${RUNNER_ENVIRONMENT:-}" != github-hosted ]]; then
    echo 'Refusing OFS /System smoke test outside disposable GitHub-hosted CI.' >&2
    exit 1
fi

app_dir=/Programs/Filesystems
helper="$app_dir/Helpers/ofs-module-helper"
source_dir="$app_dir/Sources/ofs"
module=/System/Filesystems/OFS.so
[[ ! -e "$helper" && ! -e "$module" && ! -d "$app_dir" ]] ||
  { echo "Pre-existing OFS test path; refusing replacement" >&2; exit 1; }

cleanup() {
    sudo "$helper" remove >/dev/null 2>&1 || :
    sudo rm -f "$helper"
    for part in module core efi; do
        sudo rm -f "$source_dir/$part/"*
        sudo rmdir "$source_dir/$part" >/dev/null 2>&1 || :
    done
    sudo rmdir "$source_dir" >/dev/null 2>&1 || :
    sudo rmdir "$app_dir/Sources" "$app_dir/Helpers" "$app_dir" /Programs >/dev/null 2>&1 || :
}
trap cleanup EXIT

sudo install -d -m 0755 "$app_dir/Helpers" "$source_dir/module" "$source_dir/core" "$source_dir/efi"
for type in module core efi; do
    case "$type" in
        module) names=(ofs_module.c ofs_module.h) ;;
        core) names=(ofs_core.c ofs_core.h ofs_primitives.c ofs_primitives.h) ;;
        efi) names=(reader.c reader.h) ;;
    esac
    for name in "${names[@]}"; do
        sudo install -m 0644 "native/filesystems/ofs/$type/$name" \
            "$source_dir/$type/$name"
    done
done
sudo install -m 0755 packaging/linux/ofs-module-helper.sh "$helper"

sudo "$helper" install
[[ -f "$module" ]] || { echo "missing compiled OFS.so" >&2; exit 1; }
[[ "$(sudo "$helper" status)" == installed ]] ||
  { echo "installed module not detected" >&2; exit 1; }
if readelf -d "$module" | grep 'NEEDED' >/dev/null; then
    echo "OFS.so has host-linked library dependencies" >&2
    exit 1
fi
readelf -Ws "$module" | grep 'infiltrator_filesystem_module_v1$' >/dev/null

sudo "$helper" remove
[[ ! -e "$module" ]] || { echo "OFS removal failed" >&2; exit 1; }

# The copied prebuilt module and a module compiled by Install are discovered
# through the exact same directory; neither installs a second filesystem.
prebuilt=$(find build-desktop -name OFS.so -type f -print -quit)
[[ -n "$prebuilt" ]] || { echo "prebuilt OFS.so missing" >&2; exit 1; }
sudo install -m 0644 "$prebuilt" "$module"
[[ "$(sudo "$helper" status)" == installed ]] ||
  { echo "precompiled OFS.so was not recognised" >&2; exit 1; }
sudo "$helper" remove

echo 'OFS-SELECT-COMPILE-COPY-AND-DRAG-PASS'
