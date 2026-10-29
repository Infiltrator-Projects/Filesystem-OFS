#!/usr/bin/env python3
"""Release the OFS DEB and source-embedded native RUN."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

import build as fsbuild

ROOT = Path(__file__).resolve().parents[1]
REPO = "Filesystem-OFS"
SLUG = "filesystem-ofs"
PACKAGE = "infiltrator-filesystem-ofs"
MODULE = "OFS.so"
VERSION = (ROOT / "VERSION").read_text().strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", VERSION):
    raise SystemExit("Invalid standalone VERSION")
DIST = ROOT / "dist"

def run(args):
    subprocess.run([str(a) for a in args], check=True)

def build_deb(profile, dest):
    with tempfile.TemporaryDirectory(prefix=SLUG + "-package-") as tmp:
        root = Path(tmp) / "root"
        module = root / "System/Filesystems" / MODULE
        module.parent.mkdir(parents=True)
        flags = fsbuild.build(profile, module)
        fsbuild.test(module)
        # The fixture test is an ephemeral QA executable, never a filesystem driver.
        (module.parent / "ofs-module-test").unlink()
        info_dir = root / "System/Resources/Documentation" / REPO
        info_dir.mkdir(parents=True)
        digest = hashlib.sha256(module.read_bytes()).hexdigest()
        (info_dir / "BUILD-INFO").write_text(
            f"Version: {VERSION}\nModule: /System/Filesystems/{MODULE}\n"
            f"Package: {PACKAGE}\nProfile: {profile}\n"
            f"C flags: {' '.join(flags)}\n"
            f"LTO: {'enabled' if profile == 'aggressive' else 'disabled'}\n"
            f"PGO: {'trained two-pass profile (OFS V1/V2/V3 boundary and failure fixtures)' if profile == 'aggressive' else 'disabled'}\n"
            f"SHA256: {digest}\n")
        for item in ("README.md", "MIGRATION-PROVENANCE.md", "LICENSE"):
            shutil.copy2(ROOT / item, info_dir / item)
        control = root / "DEBIAN"
        control.mkdir()
        deb_version = VERSION + ("+native1" if profile == "aggressive" else "")
        (control / "control").write_text(
            f"Package: {PACKAGE}\nVersion: {deb_version}\nArchitecture: amd64\n"
            "Section: admin\nPriority: optional\n"
            "Maintainer: Infiltrator Projects <noreply@infiltrator-projects.invalid>\n"
            "Description: Amiga Original File System shared object for InfiltratorOS\n"
            " Installs OFS.so in /System/Filesystems.\n")
        dest.parent.mkdir(parents=True, exist_ok=True)
        run(["dpkg-deb", "--root-owner-group", "--build", root, dest])
        content = subprocess.check_output(["dpkg-deb", "--contents", str(dest)], text=True)
        if f"./System/Filesystems/{MODULE}" not in content:
            raise RuntimeError("Missing selected module in package")
    return dest

def source_files():
    if (ROOT / ".git").exists():
        names = subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "-z"])
        paths = [Path(os.fsdecode(p)) for p in names.split(b"\0") if p]
    else:
        paths = [p.relative_to(ROOT) for p in ROOT.rglob("*") if p.is_file()]
    chosen = []
    for p in paths:
        q = p.as_posix()
        selected = (
            q.startswith("native/filesystems/ofs/") or
            q == "native/include/infiltratr/fs/boot_module.h" or
            q.startswith("standalone/") or q.startswith("docs/") or
            q.startswith("tests/") or q.startswith("scripts/") or q.startswith(".github/") or
            q in ("README.md", "VERSION", "MIGRATION-PROVENANCE.md", "LICENSE", "Makefile",
                  "tests/ofs_dropin_module_test.c", "tests/ofs_v3_test.py", "tests/ofs_complete_test.py")
        )
        if selected and (ROOT / p).is_file() and not (ROOT / p).is_symlink():
            chosen.append(p)
    if len(chosen) < 30:
        raise RuntimeError("Insufficient source files for full independent build")
    return sorted(set(chosen))

def create_run(path):
    with tempfile.TemporaryFile() as temp:
        with tarfile.open(fileobj=temp, mode="w:gz", format=tarfile.PAX_FORMAT) as tar:
            for p in source_files():
                data = (ROOT / p).read_bytes()
                entry = tarfile.TarInfo(p.as_posix())
                entry.size, entry.mode, entry.uid, entry.gid, entry.mtime = len(data), 0o644, 0, 0, 0
                tar.addfile(entry, io.BytesIO(data))
        temp.seek(0)
        payload = temp.read()
    digest = hashlib.sha256(payload).hexdigest()
    header = """#!/bin/sh
set -eu
if [ "$#" -gt 0 ] && [ "$1" = "--help" ]; then
    echo "Usage: __NAME__ [--profile aggressive] [--system-package-mode] [--build-only --output DIRECTORY]"
    exit 0
fi
temp=$(mktemp -d)
trap 'rm -rf -- "$temp"' EXIT HUP INT TERM
line=$(awk '/^__OFS_SOURCE_PAYLOAD_BELOW__$/ {print NR + 1; exit}' "$0")
test -n "$line"
tail -n +"$line" "$0" > "$temp/source.tar.gz"
printf '%s  %s\n' '__SHA256__' "$temp/source.tar.gz" | sha256sum -c - >/dev/null
mkdir "$temp/source"
tar -xzf "$temp/source.tar.gz" -C "$temp/source" --no-same-owner --no-same-permissions
python3 "$temp/source/standalone/release.py" install "$@"
exit $?
__OFS_SOURCE_PAYLOAD_BELOW__
""".replace("__NAME__", path.name).replace("__SHA256__", digest)
    path.write_bytes(header.encode("utf-8") + payload)
    path.chmod(0o755)

def release():
    DIST.mkdir(exist_ok=True)
    if list(DIST.iterdir()):
        raise RuntimeError("Refusing to mix release artifacts; clean dist first")
    deb = build_deb("portable", DIST / f"{PACKAGE}_{VERSION}_amd64.deb")
    runfile = DIST / f"{SLUG}-{VERSION}-native-installer.run"
    create_run(runfile)
    for artifact in (deb, runfile):
        print(artifact.name, hashlib.sha256(artifact.read_bytes()).hexdigest())
    if len(list(DIST.iterdir())) != 2:
        raise RuntimeError("Release must contain exactly the DEB and RUN")

def install(rest):
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=("portable", "aggressive"), default="aggressive")
    parser.add_argument("--system-package-mode", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--output", type=Path)
    opt = parser.parse_args(rest)
    with tempfile.TemporaryDirectory(prefix=SLUG + "-install-") as tmp:
        output = ((opt.output / f"{PACKAGE}_{VERSION}+native1_amd64.deb") if opt.build_only and opt.output else
                  Path(tmp) / f"{PACKAGE}_{VERSION}+native1_amd64.deb").resolve()
        build_deb(opt.profile, output)
        if opt.build_only:
            print(output)
            return
        args = ["dpkg", "-i", output]
        if os.geteuid() != 0:
            args.insert(0, "sudo")
        run(args)
        installed = Path("/System/Filesystems") / MODULE
        fsbuild.inspect(installed)
        record = (Path("/System/Resources/Documentation") / REPO / "BUILD-INFO").read_text()
        sha = hashlib.sha256(installed.read_bytes()).hexdigest()
        if f"SHA256: {sha}\n" not in record:
            raise RuntimeError("Installed binary does not match BUILD-INFO")
        print("Installed and verified", installed)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("command", choices=("release", "install"))
    args, rest = p.parse_known_args()
    if args.command == "release":
        if rest:
            p.error("Unrecognised release options")
        release()
    else:
        install(rest)

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f"ERROR: {exc}")
