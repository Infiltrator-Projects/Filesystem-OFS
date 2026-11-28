#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A Git checkout obtains its dependency from the gitlink. GitHub source
# archives omit submodule trees, so they carry the same native source pin.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd -P)
dep="$root/src/infiltratr-common"
pin=$(cat "$root/standalone/COMMON_REVISION")
[[ "$pin" =~ ^[0-9a-f]{40}$ ]]
verify() {
    [[ -e "$dep/.git" ]] &&
    [[ "$(git -C "$dep" rev-parse HEAD)" == "$pin" ]] &&
    git -C "$dep" diff --quiet HEAD -- include &&
    [[ -f "$dep/include/infiltratr/filesystem_module_v2.h" ]]
}
if verify; then exit 0; fi
mkdir -p "$root/build"
exec 9>"$root/build/.common-checkout.lock"
flock 9
if verify; then exit 0; fi
if [[ -e "$dep/.git" ]]; then
    echo 'The Common checkout differs from the pinned, unmodified headers.' >&2
    exit 1
fi
if [[ "$(git -C "$root" rev-parse --show-toplevel 2>/dev/null || true)" == "$root" ]]; then
    [[ "$(git -C "$root" ls-files --stage -- src/infiltratr-common | awk '{print $1, $2}')" == "160000 $pin" ]]
    git -C "$root" submodule update --init --depth=1 -- src/infiltratr-common
else
    mkdir -p "$root/src"
    work=$(mktemp -d "$root/src/.common-source.XXXXXX")
    trap 'rm -rf -- "$work"' EXIT
    git -C "$work" init -q
    git -C "$work" fetch -q --depth=1 https://github.com/Infiltrator-Projects/Libraries.git "$pin"
    git -C "$work" checkout -q --detach FETCH_HEAD
    [[ "$(git -C "$work" rev-parse HEAD)" == "$pin" ]]
    # rename() replaces an empty archive placeholder, never a populated tree.
    mv -T "$work" "$dep"
fi
verify
