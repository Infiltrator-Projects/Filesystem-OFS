#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic 512 KiB partition-relative OFS fixture for OVMF.

Equivalent DOS/0 structures to ofs_efi_reader_test.c, not a second formatter:
only a fixed test partition with a nested five-byte file is synthesized.
"""
import argparse
from pathlib import Path

BLOCKS = 1024
SECTOR = 512
ROOT = BLOCKS // 2


def put(block, word, value):
    block[word * 4:word * 4 + 4] = (value & 0xffffffff).to_bytes(4, "big")


def seal(block):
    put(block, 5, 0)
    total = sum(int.from_bytes(block[i:i + 4], "big")
                for i in range(0, SECTOR, 4))
    put(block, 5, -total)
    assert sum(int.from_bytes(block[i:i + 4], "big")
               for i in range(0, SECTOR, 4)) & 0xffffffff == 0


def name(block, value):
    data = value.encode("ascii")
    assert 0 < len(data) <= 30
    block[432] = len(data)
    block[433:433 + len(data)] = data


def bucket(value):
    data = value.upper().encode("ascii")
    result = len(data)
    for char in data:
        result = (result * 13 + char) & 0x7ff
    return result % 72


def make(path, alien):
    disk = bytearray(BLOCKS * SECTOR)

    def sector(index):
        return memoryview(disk)[index * SECTOR:(index + 1) * SECTOR]

    put(sector(0), 0, 0x444f5301 if alien else 0x444f5300)

    root = sector(ROOT)
    put(root, 0, 2)
    put(root, 3, 72)
    put(root, 127, 1)
    put(root, 6 + bucket("boot"), 4)
    name(root, "OFSBOOT")
    seal(root)

    folder = sector(4)
    put(folder, 0, 2)
    put(folder, 1, 4)
    put(folder, 125, ROOT)
    put(folder, 127, 2)
    put(folder, 6 + bucket("kernel.efi"), 5)
    name(folder, "boot")
    seal(folder)

    file = sector(5)
    put(file, 0, 2)
    put(file, 1, 5)
    put(file, 2, 1)
    put(file, 4, 6)
    put(file, 81, 5)
    put(file, 125, 4)
    put(file, 127, -3)
    name(file, "kernel.efi")
    seal(file)

    payload = sector(6)
    put(payload, 0, 8)
    put(payload, 1, 5)
    put(payload, 2, 1)
    put(payload, 3, 5)
    payload[24:29] = b"HELLO"
    seal(payload)
    Path(path).write_bytes(disk)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("--alien", action="store_true")
    args = parser.parse_args()
    make(args.path, args.alien)
