#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Test both entry points used by maintainers and source-archive consumers.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd -P)
pin=$(cat "$root/standalone/COMMON_REVISION")
tree=${1:-HEAD}
test "$(git -C "$root" ls-tree "$tree" src/infiltratr-common | cut -f1)" = "160000 commit $pin"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
git -C "$root" archive "$tree" | tar -x -C "$work"
test ! -e "$work/src/infiltratr-common/include/infiltratr/filesystem_module_v2.h"
bash "$work/standalone/prepare-common.sh"
test "$(git -C "$work/src/infiltratr-common" rev-parse HEAD)" = "$pin"
test ! -e "$work/src/infiltratr-common/include/infiltratr/filesystem_module.h"
# A real hosted build also embeds and independently inspects the firmware ELF.
make -C "$work" all
