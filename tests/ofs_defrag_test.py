#!/usr/bin/env python3
"""Exercise OFS.so through the production filesystem-neutral Defragmenter host.

Images and independent oracle output are disposable. Fault controls are compiled
only into the test executable; the installed host has no environment overrides.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from ofs_v3_test import Api, Host, Date, oracle, image
from ofs_complete_test import block, u32, fresh, space


def execute(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def build_host(source, work):
    core = source / 'src/core'
    common = source / 'shared/infiltratr-common'
    assert (common / 'VERSION').is_file(), 'Initialise the pinned Common submodule'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'version.h').write_text((core / 'version.h.in').read_text().replace(
        '@DEFRAGMENTER_PACKAGE_VERSION@', (source / 'VERSION').read_text().strip()))
    (work / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OfsModuleCompatibility C)
set(INFILTRATR_COMMON_BUILD_TESTS OFF)
set(INFILTRATR_COMMON_BUILD_SHARED OFF)
add_subdirectory("{common}" common)
find_package(OpenSSL REQUIRED)
add_executable(ofs-worker "{source}/native/ofs_module_loader.c"
 "{source}/native/filesystem_module_host.c"
 "{core}/ld_runtime.c" "{core}/ld_io.c" "{core}/ld_device.c"
 "{core}/ld_stop.c" "{core}/ld_path.c" "{core}/ld_protocol.c")
target_include_directories(ofs-worker PRIVATE "{core}" "{source}/native" "{work}")
target_compile_definitions(ofs-worker PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 IFS_MODULE_TESTING)
target_compile_options(ofs-worker PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(ofs-worker PRIVATE InfiltratrCommon::Common OpenSSL::Crypto dl pthread)
''')
    flags = []
    if os.getenv('IFS_DEFRAG_SANITIZE') == '1':
        flags = ['-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer',
                 '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
    execute(['cmake', '-S', work, '-B', work / 'build', *flags])
    execute(['cmake', '--build', work / 'build', '-j', '4'])
    return work / 'build/ofs-worker'


def fixture(api, path, bs, variant, blocks=4096):
    from amitools.fs.FSString import FSString
    image(path, bs * blocks)
    vol, dev = oracle(path, bs, True, variant)
    vol.create_dir(FSString('Foreign'))
    vol.write_file(b'independent producer', FSString('Foreign/Producer'))
    vol.close(); dev.close()
    host = Host(path)
    assert api.mount(C.byref(host.io), None) == 0
    assert api.mkdir(b'/D') == 0 and api.mkdir(b'/Other') == 0
    names = [b'/D/A', b'/D/B', b'/D/C']
    for name in names:
        assert api.create(name) == 0
    payload = bs - 24
    # Repeated append to interleaved files forces noncontiguous data chains.
    for index in range(17):
        for number, name in enumerate(names):
            data = bytes([65 + number + index]) * payload
            assert api.write(name, index * payload, data, len(data)) == 0
    for name in names:
        assert api.set_owner(name, 123, 456) == 0
        assert api.set_dates(name, C.byref(Date(1234, 567, 890)), 1) == 0
        assert api.set_comment(name, b'preserved comment') == 0
    assert api.create(b'/empty') == 0
    if variant == 6:
        long_name = b'/' + b'N' * 106
        assert api.create(long_name) == 0 and api.set_comment(long_name, b'c' * 79) == 0
    assert api.hardlink(b'/D/A', b'/Other/Alias') == 0
    assert api.hardlink(b'/D', b'/Other/Directory') == 0
    assert api.hardlink(b'/D', b'/D/Cycle') == 0
    assert api.symlink(b'D/A', b'/Symbol') == 0
    # At 512-byte blocks cross two reversed-pointer extension tables.
    if bs == 512:
        assert api.create(b'/large') == 0
        data = bytes((i * 31 + 7) & 255 for i in range(payload * 151 + 37))
        assert api.write(b'/large', 0, data, len(data)) == 0
    api.check(); assert api.unmount() == 0; host.close()


def snapshot(api, path):
    host = Host(path); assert api.mount(C.byref(host.io), None) == 0
    names = [b'/', b'/Foreign', b'/Foreign/Producer', b'/D', b'/D/A', b'/D/B',
             b'/D/C', b'/Other', b'/Other/Alias', b'/Other/Directory/A', b'/D/Cycle/B',
             b'/Symbol', b'/empty']
    state = {}
    for name in names + [b'/large', b'/' + b'N' * 106]:
        try:
            e = api.meta(name)
        except AssertionError:
            assert name in (b'/large', b'/' + b'N' * 106)
            continue
        state[name] = (e.type, e.bytes, e.protection, e.uid, e.gid, e.name, e.comment,
                       tuple((d.days, d.minutes, d.ticks) for d in
                             (e.changed, e.volume_changed, e.created)),
                       api.data(name) if e.type in (-3, -4, 3) else None)
    api.check(); assert api.unmount() == 0; host.close()
    return state


def verify_independent(path, bs, api):
    from amitools.fs.FSString import FSString
    # amitools 0.8.1 cannot enumerate native hard/soft links. In a disposable
    # copy only, remove those entries through V3 before asking its independent
    # parser to inspect the retained originals. The full linked image is
    # separately checked, including both cyclic and ordinary directory aliases.
    view = path.with_suffix('.oracle-view')
    shutil.copyfile(path, view)
    host = Host(view); assert api.mount(C.byref(host.io), None) == 0
    for name in (b'/Symbol', b'/Other/Directory', b'/D/Cycle', b'/Other/Alias'):
        assert api.unlink(name) == 0
    api.check(); assert api.unmount() == 0; host.close()
    vol, dev = oracle(view, bs)
    assert vol.read_file(FSString('Foreign/Producer')) == b'independent producer'
    for number, name in enumerate(('A', 'B', 'C')):
        expected = b''.join(bytes([65 + number + i]) * (bs - 24) for i in range(17))
        assert vol.read_file(FSString('D/' + name)) == expected
        entry = vol.get_file_path_name(FSString('D/' + name))
        assert entry.get_meta_info().get_comment().get_ami_str() == b'preserved comment'
    if bs == 512:
        assert vol.read_file(FSString('large')) == bytes((i * 31 + 7) & 255 for i in range((bs - 24) * 151 + 37))
    vol.close(); dev.close(); view.unlink()


def verify_gaps(api, path, bs, analysis):
    free = set()
    for start, end in analysis['free_ranges']:
        free.update(range(start, end))
    host = Host(path); assert api.mount(C.byref(host.io), None) == 0
    for name in (b'/Foreign/Producer', b'/D/A', b'/D/B', b'/D/C', b'/large'):
        if name == b'/large' and bs != 512:
            continue
        e = api.meta(name); count = (e.bytes + bs - 25) // (bs - 24)
        key = u32(block(path, e.block, bs), 16)
        chain = []
        while key:
            chain.append(key); key = u32(block(path, key, bs), 16)
        assert len(chain) == count and all(b == a + 1 for a, b in zip(chain, chain[1:])), (name, chain)
        assert all(k in free for k in range(chain[-1] + 1, chain[-1] + 1 + (count + 9) // 10)), name
    assert api.unmount() == 0; host.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--defragmenter-source', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--worker', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = args.defragmenter_source.resolve() / 'defragger'
    sys.path.insert(0, str(args.oracle.resolve()))
    assert (root / 'native/include/infiltratr/fs/defrag_module.h').read_bytes() == (
        source / 'native/filesystem_defrag_abi.h').read_bytes(), 'Defragmenter ABI drift'
    assert (root / 'native/filesystems/ofs/module/ofs_v3.h').read_bytes() == (
        source / 'native/ofs_module_abi.h').read_bytes(), 'Test Media V3 ABI drift'
    module = args.module.resolve(); api = Api(module)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='ifs-ofs-defrag-') as temporary:
        work = Path(temporary); installed = work / 'Filesystems'; installed.mkdir()
        shutil.copyfile(module, installed / 'OFS.so')
        worker = args.worker.resolve() if args.worker else build_host(source, work / 'host')
        environment = dict(os.environ, IFS_TEST_MODULE_PARENT=str(work))

        def operation(command, path, name, extra=None, expected=0):
            journal = work / (path.stem + '.journal')
            result = subprocess.run([str(worker), command, str(path), '--write', '--confirm',
                str(path), '--journal', str(journal), '--live-updates'], env=dict(environment, **(extra or {})),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
            (output / (name + '.log')).write_text(result.stdout + result.stderr)
            assert result.returncode == expected, (name, result.returncode, result.stdout, result.stderr)
            return result, journal

        def analyse(path):
            result, _ = operation('analyse-json', path, path.stem + '-analyse')
            return json.loads(result.stdout)

        for bs in ((512,) if args.quick else (512, 1024, 2048, 4096, 8192, 16384, 32768)):
            for variant in (0, 2, 4, 6):
                path = work / f'ofs-{bs}-{variant}.img'
                fixture(api, path, bs, variant); before = snapshot(api, path)
                assert analyse(path)['fragmented_files'] >= 3
                for command in ('defrag', 'growth-defrag'):
                    operation(command, path, path.stem + '-' + command)
                    assert snapshot(api, path) == before, (bs, variant, command, 'metadata/content changed')
                    verify_independent(path, bs, api)
                    after = analyse(path); assert after['fragmented_files'] == 0
                    if command == 'growth-defrag': verify_gaps(api, path, bs, after)
                    same = digest(path)
                    operation(command, path, path.stem + '-' + command + '-again')
                    assert digest(path) == same, (bs, variant, 'non-idempotent', command)
                print('OFS-DEFRAG-INDEPENDENT-LINKS-METADATA-GROWTH-PASS', bs, variant, flush=True)

        baseline = work / 'baseline.img'; fixture(api, baseline, 512, 6)
        wanted = snapshot(api, baseline)
        path = work / 'bitmap-extension.img'; fixture(api, path, 512, 6, 110000)
        extended = snapshot(api, path)
        operation('growth-defrag', path, 'bitmap-extension')
        assert snapshot(api, path) == extended
        verify_independent(path, 512, api)
        path = work / 'full.img'
        host = fresh(api, path, blocks=128)
        assert api.create(b'/full') == 0
        # Fill using the actual reported free allocation; no free blocks are
        # available for a Growth reserve or for private metadata scratch.
        remaining = space(api).free // 512
        data = bytes(488 * min(remaining, 70))
        assert api.write(b'/full', 0, data, len(data)) == 0
        while space(api).free:
            e = api.meta(b'/full')
            r = api.write(b'/full', e.bytes, bytes(488), 488)
            if r != 0: break  # the last pointer-table extension also needs space
        api.check(); assert api.unmount() == 0; host.close()
        unchanged = digest(path)
        operation('growth-defrag', path, 'full-refused', expected=1)
        assert digest(path) == unchanged
        for phase in range(1, 4):
            path = work / f'phase-{phase}.img'; shutil.copyfile(baseline, path)
            _, journal = operation('defrag', path, path.stem,
                                   {'IFS_TEST_CRASH_AFTER_JOURNAL': str(phase)}, 86)
            assert journal.exists()
            wrong = work / 'wrong.img'; shutil.copyfile(path, wrong); unchanged = digest(wrong)
            # Journal path is intentionally that of the original transaction.
            result = subprocess.run([str(worker), 'recover', str(wrong), '--write', '--confirm',
                str(wrong), '--journal', str(journal)], env=environment, capture_output=True)
            assert result.returncode == 1 and digest(wrong) == unchanged and journal.exists()
            operation('recover', path, path.stem + '-recover')
            assert snapshot(api, path) == wanted and not journal.exists()
        crashes = 0
        for write in range(1, 9):
            path = work / f'write-{write}.img'; shutil.copyfile(baseline, path)
            result = subprocess.run([str(worker), 'growth-defrag', str(path), '--write', '--confirm',
                str(path), '--journal', str(work / (path.stem + '.journal'))],
                env=dict(environment, IFS_TEST_CRASH_AFTER_DEVICE_WRITE=str(write)), capture_output=True)
            assert result.returncode in (0, 87), (write, result.stderr)
            if result.returncode == 87:
                crashes += 1; operation('recover', path, path.stem + '-recover')
            assert snapshot(api, path) == wanted
        assert crashes >= 2
        path = work / 'stop.img'; shutil.copyfile(baseline, path)
        _, journal = operation('defrag', path, 'stop', {'IFS_TEST_STOP_AFTER_DEVICE_WRITE': '1'}, 130)
        assert journal.exists(); operation('recover', path, 'stop-recover')
        assert snapshot(api, path) == wanted
        path = work / 'changed-source.img'; shutil.copyfile(baseline, path)
        _, journal = operation('defrag', path, 'changed-source', {'IFS_TEST_CRASH_AFTER_JOURNAL': '1'}, 86)
        host = Host(path); assert api.mount(C.byref(host.io), None) == 0
        assert api.write(b'/D/A', 0, b'NEW', 3) == 0
        assert api.unmount() == 0; host.close(); unchanged = digest(path)
        operation('recover', path, 'changed-source-refused', expected=1)
        assert digest(path) == unchanged and journal.exists()
        path = work / 'bad-stage.img'; shutil.copyfile(baseline, path)
        _, journal = operation('defrag', path, 'bad-stage', {'IFS_TEST_CRASH_AFTER_JOURNAL': '2'}, 86)
        stage = Path(str(journal) + '.ofs-stage')
        with stage.open('r+b') as file: file.seek(200); file.write(b'corruption')
        unchanged = digest(path)
        operation('recover', path, 'bad-stage-refused', expected=1)
        assert journal.exists() and digest(path) == unchanged
        for bad, content in (('foreign', b'DOS\x01'), ('truncated', b'DOS\0')):
            path = work / (bad + '.img'); path.write_bytes(content); unchanged = digest(path)
            operation('defrag', path, bad, expected=1); assert digest(path) == unchanged
        (installed / 'OFS.so').rename(installed / 'unavailable')
        unchanged = digest(baseline)
        operation('defrag', baseline, 'missing-module', expected=1)
        assert digest(baseline) == unchanged
    print('OFS-SO-DEFRAGMENTER-WRITE-RECOVERY-PASS', digest(module), flush=True)


if __name__ == '__main__':
    main()
