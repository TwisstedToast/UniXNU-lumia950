#!/usr/bin/env python3
"""Validate the architecture, entry point, and subsystem of the diagnostic EFI app."""

import argparse
from pathlib import Path
import struct


def check_image(data: bytes) -> dict[str, int]:
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("missing DOS header")
    pe_offset = struct.unpack_from("<I", data, 60)[0]
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    machine, section_count, _, _, _, optional_size, characteristics = struct.unpack_from(
        "<HHIIIHH", data, pe_offset + 4
    )
    if machine != 0xAA64:
        raise ValueError("EFI application is not AArch64")
    if characteristics & 1:
        raise ValueError("relocations are stripped")
    optional_offset = pe_offset + 24
    section_offset = optional_offset + optional_size
    if optional_size < 112 or section_offset + section_count * 40 > len(data):
        raise ValueError("truncated headers")
    if struct.unpack_from("<H", data, optional_offset)[0] != 0x20B:
        raise ValueError("EFI application is not PE32+")
    entry_point = struct.unpack_from("<I", data, optional_offset + 16)[0]
    subsystem = struct.unpack_from("<H", data, optional_offset + 68)[0]
    if subsystem != 10:
        raise ValueError("PE subsystem is not EFI_APPLICATION")
    entry_found = False
    for section_index in range(section_count):
        offset = section_offset + section_index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        flags = struct.unpack_from("<I", data, offset + 36)[0]
        if raw_size and (raw_offset < section_offset + section_count * 40 or
                         raw_offset + raw_size > len(data)):
            raise ValueError("section data is outside the file")
        if (virtual_address <= entry_point < virtual_address + virtual_size and
                entry_point - virtual_address < raw_size and flags & 0x20000000):
            entry_found = True
    if not entry_point or not entry_found:
        raise ValueError("entry point is not backed by executable section data")
    return {"bytes": len(data), "entry_point": entry_point, "sections": section_count}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    arguments = parser.parse_args()
    try:
        result = check_image(arguments.image.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"AArch64 EFI application: {result['bytes']} bytes, "
          f"entry RVA 0x{result['entry_point']:x}, {result['sections']} sections")


if __name__ == "__main__":
    main()
