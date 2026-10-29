#!/usr/bin/env python3
"""Fail-closed regression tests for image and guarded partition OFS formatting."""
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile


def command(tool, *args, success=True):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True)
    if (result.returncode == 0) != success:
        raise AssertionError(
            f"unexpected status {result.returncode}: {args}\n"
            f"stdout={result.stdout}\nstderr={result.stderr}"
        )


def damage(image, offset):
    with image.open("r+b") as fh:
        fh.seek(offset)
        before = fh.read(1)
        fh.seek(offset)
        fh.write(bytes((before[0] ^ 0x80,)))


def test(tool, root):
    for size in (1, 32, 64, 2048):  # 64 MiB+ exercises bitmap-extension traversal
        path = root / f"ofs-{size}.img"
        command(tool, "--create", path, size, "LD_AMIGA")
        assert path.stat().st_size == size * (1 << 20)
        command(tool, "--verify", path)
        command(tool, "--create", path, size, "LD_AMIGA", success=False)
        command(tool, "--verify", path)

    # All seven documented OFS block sizes have distinct root/hash/tail offsets.
    # These tests write only disposable regular-file images, never devices.
    for block in (512, 1024, 2048, 4096, 8192, 16384, 32768):
        image = root / f"ofs-block-{block}.img"
        command(tool, "--create", image, 8, "BLOCK_TEST",
                "--block-size", block)
        command(tool, "--verify", image)
        with image.open("rb") as fh:
            fh.seek((image.stat().st_size // block // 2) * block)
            contents = fh.read(block)
        words = block // 4
        assert struct.unpack_from(">I", contents, 12)[0] == words - 56
        assert struct.unpack_from(">I", contents, (words - 50) * 4)[0] == 0xffffffff
        assert struct.unpack_from(">I", contents, (words - 1) * 4)[0] == 1
        assert contents[block - 80] == len("BLOCK_TEST")
        command(tool, "--create", image, 8, "BLOCK_TEST",
                "--block-size", block, success=False)
    # Each actual on-disk variant must be independently identifiable and
    # structurally complete, not a signature-only re-label of DOS0.
    for block in (512, 1024, 2048, 4096, 8192, 16384, 32768):
        for selector, suffix in (("dos0", 0), ("dos2", 2), ("dos4", 4), ("dos6", 6)):
            image = root / f"ofs-{selector}-block-{block}.img"
            opts = ("--variant", selector, "--block-size", block) if suffix == 4 \
                else ("--block-size", block, "--variant", selector)
            command(tool, "--create", image, 4, "VARIANT", *opts)
            command(tool, "--verify", image)
            with image.open("rb") as fh:
                assert fh.read(4) == b"DOS" + bytes([suffix])
                total = image.stat().st_size // block
                key = (2 + total - 1) // 2
                fh.seek(key * block)
                header = fh.read(block)
                words = block // 4
                cache = struct.unpack_from(">I", header, block - 8)[0]
                if suffix == 6:
                    used = struct.unpack_from(">I", header, block - 44)[0]
                    dostype = struct.unpack_from(">I", header, block - 16)[0]
                    bitmap_count = ((total - 2) + (8 * block - 32) - 1) // (8 * block - 32)
                    extra = (max(0, bitmap_count - 25) + (block // 4 - 1) - 1) // (block // 4 - 1)
                    assert used == 1 + bitmap_count + extra, (block, used)
                    assert dostype == 0x444F5306
                    assert header[block - 80] == len("VARIANT")
                    assert sum(struct.unpack(f">{words}I", header)) % (1 << 32) == 0
                else:
                    assert struct.unpack_from(">I", header, block - 44)[0] == 0
                    assert struct.unpack_from(">I", header, block - 16)[0] == 0
                if suffix != 4:
                    assert cache == 0, (selector, block, cache)
                else:
                    assert 2 <= cache < total and cache != key
                    fh.seek(cache * block)
                    record = fh.read(block)
                    assert struct.unpack_from(">5I", record, 0) == (
                        33, cache, key, 0, 0)
                    assert sum(struct.unpack(f">{words}I", record)) % (1 << 32) == 0
                    bitmap = struct.unpack_from(">I", header, block - 196)[0]
                    assert bitmap not in (0, cache, key)
                    relative = cache - 2
                    fh.seek(bitmap * block + 4 + 4 * (relative // 32))
                    bitmap_word = struct.unpack(">I", fh.read(4))[0]
                    assert bitmap_word & (1 << (relative % 32)) == 0
            if suffix == 6:
                # Either LNFS-specific root field must invalidate the volume.
                for field in (block - 44, block - 16):
                    victim = root / f"bad-dos6-root-{block}-{field}.img"
                    shutil.copyfile(image, victim)
                    damage(victim, key * block + field)
                    command(tool, "--verify", victim, success=False)
            if suffix == 4:
                # Cache integrity and ownership must be verified; rejecting
                # mutated media proves DOS4 isn't only a different DOS byte.
                victim = root / f"bad-dos4-cache-{block}.img"
                shutil.copyfile(image, victim)
                damage(victim, cache * block + 8)
                command(tool, "--verify", victim, success=False)

    for bad_variant in ("dos1", "dos3", "dos5", "dos7", "DOS2", ""):
        invalid_image = root / ("bad-variant-" + (bad_variant or "empty") + ".img")
        command(tool, "--create", invalid_image, 4, "INVALID",
                "--variant", bad_variant, success=False)
        assert not invalid_image.exists()
    # Old CLI remains DOS0 by default, with no silent variant conversion.
    default_image = root / "ofs-1.img"
    with default_image.open("rb") as fh:
        assert fh.read(4) == b"DOS\x00"

    # Authentic Amiga root placement differs from floor(total/2) for
    # odd total-block counts. Exercise every block size with an independent
    # raw-byte inspection, not just the formatter's built-in verifier.
    for block in (512, 1024, 2048, 4096, 8192, 16384, 32768):
        total_blocks = (1024 * 1024 // block) + 1
        assert total_blocks % 2 == 1
        expected_root = (2 + total_blocks - 1) // 2
        image = root / f"ofs-odd-{block}.img"
        command(tool, "--create-blocks", image, total_blocks, "ODD_OFS",
                "--block-size", block)
        command(tool, "--verify", image)
        assert image.stat().st_size == total_blocks * block
        with image.open("rb") as fh:
            fh.seek(expected_root * block)
            correct = fh.read(block)
            fh.seek((total_blocks // 2) * block)
            old_position = fh.read(block)
        assert struct.unpack_from(">I", correct, 0)[0] == 2
        assert struct.unpack_from(">I", correct, 12)[0] == block // 4 - 56
        assert struct.unpack_from(">I", correct, block - 4)[0] == 1
        assert struct.unpack_from(">I", correct, block - 200)[0] == 0xffffffff
        assert correct[block - 80] == len("ODD_OFS")
        assert sum(struct.unpack(f">{block // 4}I", correct)) % (1 << 32) == 0
        assert struct.unpack_from(">I", old_position, 0)[0] != 2
        # A checksum-valid root at the former floor(N/2) location must
        # never make a malformed odd-block image look fully compliant.
        legacy = root / f"ofs-odd-legacy-{block}.img"
        shutil.copyfile(image, legacy)
        with legacy.open("r+b") as fh:
            fh.seek((total_blocks // 2) * block)
            fh.write(correct)
            fh.seek(expected_root * block)
            fh.write(bytes(block))
        command(tool, "--verify", legacy, success=False)

    for bad_block in (256, 1000, 65536, 0, -1):
        command(tool, "--create", root / f"bad-block-{bad_block}.img",
                4, "BAD_BLOCK", "--block-size", bad_block, success=False)

    # This sparse image crosses the former 1-TiB/INT32_MAX boundary.
    # It validates formatter geometry only, not Linux or EFI mount support.
    over_tib = root / "ofs-over-1tib.img"
    command(tool, "--create", over_tib, 1100 * 1024, "HIGH_BLOCK",
            "--block-size", 4096)
    command(tool, "--verify", over_tib)
    assert over_tib.stat().st_size > (1 << 40)

    # Full SD-card-class volume: sparse 120-GiB image with every bitmap
    # extension populated. The formatter allocates only metadata, not 120 GiB
    # of backing storage, and performs its own full-volume verification.
    large_mib = 120 * 1024
    large = root / "ofs-120g.img"
    command(tool, "--create", large, large_mib, "FULL_SD_OFS")
    assert large.stat().st_size == large_mib * (1 << 20)
    with large.open("rb") as fh:
        fh.seek((large.stat().st_size // 512 // 2) * 512)
        large_root = fh.read(512)
        assert struct.unpack_from(">I", large_root, 3 * 4)[0] == 72
        assert struct.unpack_from(">I", large_root, 104 * 4)[0] != 0, (
            "large filesystem must use bitmap extensions"
        )
    # A second, independent verifier pass must accept the complete volume.
    command(tool, "--verify", large)

    # A real DOS0 user directory has three zeroed reserved header words at
    # offsets 8, 12, and 16.  Unlike ST_ROOT, its word at offset 12 is NOT
    # the hash-table size (72), even though it contains 72 hash buckets.
    # Construct a valid populated namespace directly on a disposable image,
    # including checksummed root/directory and an updated allocation bitmap.
    populated = root / "ofs-user-directory.img"
    command(tool, "--create", populated, 1, "USERDIR")
    directory_key = 3
    root_key = 1024
    name = b"boot"
    bucket = len(name)
    for ch in name:
        bucket = (bucket * 13 + (ch - 32 if 97 <= ch <= 122 else ch)) & 0x7ff
    bucket %= 72

    def set_checksum(data):
        struct.pack_into(">I", data, 20, 0)
        total = sum(struct.unpack(">128I", data)) & 0xffffffff
        struct.pack_into(">I", data, 20, (-total) & 0xffffffff)

    with populated.open("r+b") as fh:
        fh.seek(root_key * 512)
        root_data = bytearray(fh.read(512))
        bitmap_key = struct.unpack_from(">I", root_data, 79 * 4)[0]
        assert bitmap_key == 2, bitmap_key
        assert struct.unpack_from(">I", root_data, (6 + bucket) * 4)[0] == 0
        struct.pack_into(">I", root_data, (6 + bucket) * 4, directory_key)
        set_checksum(root_data)
        fh.seek(root_key * 512)
        fh.write(root_data)

        directory = bytearray(512)
        struct.pack_into(">I", directory, 0, 2)  # T_HEADER
        struct.pack_into(">I", directory, 4, directory_key)
        struct.pack_into(">I", directory, 125 * 4, root_key)  # Parent
        struct.pack_into(">I", directory, 127 * 4, 2)  # ST_USERDIR
        directory[432] = len(name)
        directory[433:433 + len(name)] = name
        set_checksum(directory)
        fh.seek(directory_key * 512)
        fh.write(directory)

        fh.seek(bitmap_key * 512)
        bitmap = bytearray(fh.read(512))
        # Bitmap entry zero corresponds to volume block 2, so block 3 is bit 1.
        bitmap_word = struct.unpack_from(">I", bitmap, 4)[0]
        assert bitmap_word & 2
        struct.pack_into(">I", bitmap, 4, bitmap_word & ~2)
        struct.pack_into(">I", bitmap, 0, 0)
        total = sum(struct.unpack(">128I", bitmap)) & 0xffffffff
        struct.pack_into(">I", bitmap, 0, (-total) & 0xffffffff)
        fh.seek(bitmap_key * 512)
        fh.write(bitmap)

    command(tool, "--verify", populated)
    for reserved_word in (2, 3, 4):
        bad = root / f"ofs-user-directory-reserved-{reserved_word}.img"
        shutil.copyfile(populated, bad)
        with bad.open("r+b") as fh:
            fh.seek(directory_key * 512)
            data = bytearray(fh.read(512))
            struct.pack_into(">I", data, reserved_word * 4, 72)
            set_checksum(data)
            fh.seek(directory_key * 512)
            fh.write(data)
        command(tool, "--verify", bad, success=False)

    existing = root / "existing.bin"
    existing.write_bytes(b"USER DATA MUST NOT CHANGE")
    command(tool, "--create", existing, 4, success=False)
    assert existing.read_bytes() == b"USER DATA MUST NOT CHANGE"
    link = root / "existing-link"
    link.symlink_to(existing)
    command(tool, "--create", link, 4, success=False)
    command(tool, "--verify", link, success=False)
    assert existing.read_bytes() == b"USER DATA MUST NOT CHANGE"

    for args in [
        ("0",), ("2097152",), ("18446744073709551616",),
        ("1.5",), ("-1",),
    ]:
        command(tool, "--create", root / ("bad-" + args[0]), *args,
                success=False)
    command(tool, "--create", root / "bad-label", 4, "bad:name", success=False)
    command(tool, "--create", root / "long-label", 4, "x" * 31, success=False)
    command(tool, "--create", root / "empty-label", 4, "", success=False)

    healthy = root / "ofs-1.img"
    with healthy.open("rb") as fh:
        fh.seek((1 << 20) // 2)
        # root block is located at the centre of the volume
        fh.seek(1024 * 512)
        rootblock = fh.read(512)
    pointer = struct.unpack_from(">I", rootblock, 79 * 4)[0]
    assert 2 <= pointer < 2048
    cases = {
        "boot": 0,
        "root checksum": 1024 * 512 + 5 * 4,
        "bitmap checksum": pointer * 512,
        "bitmap bit": pointer * 512 + 8,
        "directory size": 1024 * 512 + 3 * 4,
    }
    for label, offset in cases.items():
        victim = root / f"corrupt-{label}.img"
        shutil.copyfile(healthy, victim)
        damage(victim, offset)
        command(tool, "--verify", victim, success=False)

    # No device should be accepted: creation is exclusive, verification is file-only.
    if os.path.exists("/dev/null"):
        command(tool, "--verify", "/dev/null", success=False)
        command(tool, "--create", "/dev/null", 4, success=False)
        command(tool, "--verify-partition", "/dev/null", success=False)
        command(tool, "--format-partition", "/dev/null",
                "TestOFS", "--confirm", "/dev/null", success=False)
        command(tool, "--format-partition", "/dev/null",
                "TestOFS", "--confirm", "/dev/zero", success=False)
        command(tool, "--format-partition", "/dev/null",
                "bad:name", "--confirm", "/dev/null", success=False)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="ofs-image-test-") as tmp:
        test(pathlib.Path(sys.argv[1]), pathlib.Path(tmp))
    print("OFS image creation, extension geometry and failure-safety checks: PASS")
