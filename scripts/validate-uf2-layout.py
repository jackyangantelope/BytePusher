#!/usr/bin/env python3
"""Validate the Wave 1 application UF2 block sequence and address layout."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


UF2_BLOCK_SIZE = 512
UF2_PAYLOAD_SIZE = 256
UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30


def parse_int(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("uf2", type=Path)
    parser.add_argument("--app-base", type=parse_int, default=0x10080000)
    parser.add_argument("--metadata", type=parse_int, default=0x10FFFF00)
    args = parser.parse_args()

    raw = args.uf2.read_bytes()
    if not raw or len(raw) % UF2_BLOCK_SIZE:
        raise SystemExit("invalid UF2 length")

    total = len(raw) // UF2_BLOCK_SIZE
    app_records: list[tuple[int, int, int]] = []
    metadata_count = 0
    for index in range(total):
        block = raw[index * UF2_BLOCK_SIZE : (index + 1) * UF2_BLOCK_SIZE]
        magic0, magic1, _, target, payload, block_no, num_blocks, _ = (
            struct.unpack_from("<8I", block)
        )
        magic_end = struct.unpack_from("<I", block, 508)[0]
        if (magic0, magic1, magic_end) != (
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_MAGIC_END,
        ):
            raise SystemExit(f"invalid magic at block {index}")
        if payload != UF2_PAYLOAD_SIZE:
            raise SystemExit(f"invalid payload size at block {index}: {payload}")
        if target == args.metadata:
            metadata_count += 1
        else:
            app_records.append((target, block_no, num_blocks))

    app_targets = [record[0] for record in app_records]
    expected = [
        args.app_base + i * UF2_PAYLOAD_SIZE for i in range(len(app_targets))
    ]
    if app_targets != expected:
        raise SystemExit("application targets are not contiguous from app base")
    if [record[1] for record in app_records] != list(range(len(app_records))):
        raise SystemExit("application block numbers are not sequential")
    if any(record[2] != len(app_records) for record in app_records):
        raise SystemExit("application num_blocks does not match application count")
    if metadata_count != 1:
        raise SystemExit(f"expected one metadata block, found {metadata_count}")

    last = app_targets[-1]
    print(
        "UF2 layout: PASS "
        f"total={total} app={len(app_targets)} "
        f"range=0x{app_targets[0]:08X}..0x{last:08X} "
        f"end=0x{last + UF2_PAYLOAD_SIZE:08X} metadata=0x{args.metadata:08X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
