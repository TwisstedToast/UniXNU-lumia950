import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]


def pe_word(blob, address):
    if address < 0 or address & 3:
        raise ValueError('unaligned or negative instruction address')
    if len(blob) < 64 or blob[:2] != b'MZ':
        raise ValueError('invalid DOS header')
    offset = struct.unpack_from('<I', blob, 60)[0]
    if offset > len(blob) - 24 or blob[offset:offset + 4] != b'PE\0\0':
        raise ValueError('invalid PE header')
    machine, count = struct.unpack_from('<HH', blob, offset + 4)
    optional_bytes = struct.unpack_from('<H', blob, offset + 20)[0]
    optional = offset + 24
    sections = optional + optional_bytes
    if machine != 0xaa64 or not 1 <= count <= 32 or optional_bytes < 32 or \
        sections + count * 40 > len(blob) or struct.unpack_from('<H', blob, optional)[0] != 0x20b:
        raise ValueError('invalid ARM64 PE32+ layout')
    base = struct.unpack_from('<Q', blob, optional + 24)[0]
    relative = address - base
    matches = []
    for index in range(count):
        virtual_bytes, virtual, raw_bytes, raw = struct.unpack_from('<IIII', blob, sections + index * 40 + 8)
        within = relative - virtual
        if within < 0 or within + 4 > virtual_bytes or within + 4 > raw_bytes:
            continue
        if raw + within + 4 > len(blob):
            raise ValueError('instruction outside backing file')
        matches.append(struct.unpack_from('<I', blob, raw + within)[0])
    if len(matches) != 1:
        raise ValueError('instruction is not uniquely file backed')
    return matches[0]


def validate_source(evidence):
    header = (ROOT / 'drivers/NativeAdreno418/SecureSmmu.h').read_text()
    for name, expected in evidence['native_constants'].items():
        found = re.search(rf'constexpr u32 {name}\s*=\s*(0x[0-9a-f]+|[0-9]+);', header)
        if not found or int(found[1], 0) != expected:
            raise ValueError('native SCM constant does not match the OEM audit: ' + name)


def main():
    parser = argparse.ArgumentParser(description='Check the native SCM constants against the audited OEM TrEE transport')
    parser.add_argument('--tree-driver', type=Path, help='Optional exact private handset driver; never fetched or executed')
    parser.add_argument('--firmware-root', type=Path, help='Optional pinned public firmware source for static EL2 patch evidence')
    args = parser.parse_args()
    evidence = json.loads((ROOT / 'docs/hardware/2026-10-27-oem-scm-transport.json').read_text())
    validate_source(evidence)
    verified_words = 0
    if args.tree_driver:
        blob = args.tree_driver.read_bytes()
        driver = evidence['tree_driver']
        if len(blob) != driver['bytes'] or hashlib.sha256(blob).hexdigest() != driver['sha256']:
            raise ValueError('OEM driver does not match the captured handset binary')
        for address, expected in driver['verified_words'].items():
            if pe_word(blob, int(address, 16)) != int(expected, 16):
                raise ValueError('OEM transport word mismatch: ' + address)
            verified_words += 1
    if args.firmware_root:
        for name, expected in evidence['firmware']['files'].items():
            if hashlib.sha256((args.firmware_root / name).read_bytes()).hexdigest() != expected:
                raise ValueError('unqualified firmware source: ' + name)
    print(json.dumps({'native_constants_match_oem_audit': True,
        'private_driver_hash_checked': bool(args.tree_driver), 'driver_words_checked': verified_words,
        'pinned_el2_source_checked': bool(args.firmware_root), 'live_hypervisor_checked': False,
        'handset_secure_call_executed': False, 'handset_reset_fix_established': False}, indent=2))


if __name__ == '__main__':
    main()
