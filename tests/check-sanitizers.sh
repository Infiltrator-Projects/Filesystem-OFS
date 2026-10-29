#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mkdir -p build-sanitize
python3 - <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,'standalone')
import build
build.compile_module(Path('build-sanitize/OFS.so').resolve(),[
    '-std=c11','-O1','-g','-fPIC','-fvisibility=hidden','-mno-red-zone','-fno-builtin',
    '-fno-tree-loop-distribute-patterns','-fsanitize=address,undefined','-fno-omit-frame-pointer'],training=True)
PY
# The diagnostic DSO intentionally uses sanitizer runtimes. The final portable
# and native release objects separately must pass the zero-import inspection.
export LD_PRELOAD
LD_PRELOAD=$("${CC:-cc}" -print-file-name=libasan.so)
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
export UBSAN_OPTIONS=halt_on_error=1
python3 tests/ofs_complete_test.py --module build-sanitize/OFS.so --sanitized
