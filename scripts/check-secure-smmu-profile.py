import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def device_source(text):
    matches = list(re.finditer(r'Device\s*\(MMU3\)\s*\{', text))
    if len(matches) != 1:
        raise ValueError('expected one original MMU3 device declaration')
    match = matches[0]
    opening = text.index('{', match.start())
    depth = 0
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|[{}]', text[opening:]):
        depth += (token[0] == '{') - (token[0] == '}')
        if depth == 0:
            return text[match.start():opening + token.end()]
    raise ValueError('unterminated MMU3 device declaration')


def device_bytes(blob):
    matches = []
    for offset in range(36, len(blob) - 8):
        if blob[offset:offset + 2] != b'\x5b\x82':
            continue
        first = blob[offset + 2]
        following = first >> 6
        length = first & (15 if following else 63)
        if offset + 3 + following + 4 > len(blob):
            continue
        for index in range(following):
            length |= blob[offset + 3 + index] << (4 + index * 8)
        start = offset + 3 + following
        stop = offset + 2 + length
        if blob[start:start + 4] == b'MMU3' and stop <= len(blob):
            matches.append(blob[offset:stop])
    if len(matches) != 1:
        raise ValueError('expected one compiled MMU3 Device object')
    return matches[0]


def without_dependency(block):
    changed, count = re.subn(r'\s*Name\s*\(_DEP,\s*Package\s*\(0x01\)\s*(?://[^\n]*\n)?'
        r'\s*\{\s*\\_SB\.PEP0\s*\}\)', '', block)
    if count != 1:
        raise ValueError('expected exactly one pinned MMU3 PEP0 dependency declaration')
    return changed


def main():
    parser = argparse.ArgumentParser(description='Bind native secure SMMU constants and AML fixture to the pinned Lumia source')
    parser.add_argument('--firmware-root', required=True, type=Path)
    parser.add_argument('--iasl', help='Optionally independently recompile both exact allowlisted device variants')
    args = parser.parse_args()
    evidence = json.loads((ROOT / 'docs/hardware/2026-10-27-secure-gpu-smmu-profile.json').read_text())
    source = (args.firmware_root / evidence['acpi_source']).read_bytes()
    if hashlib.sha256(source).hexdigest() != evidence['acpi_source_sha256']:
        raise ValueError('unqualified original ACPI source')
    block = device_source(source.decode())
    recognition = evidence['profile_recognition']
    handset = recognition['handset_without_dependency']
    variants = [('kMmu3', 'secure-smmu-profile.hex', recognition, block),
        ('kMmu3Handset', 'secure-smmu-handset-profile.hex', handset, without_dependency(block))]
    code = (ROOT / 'drivers/NativeAdreno418/SecureSmmu.cpp').read_text()
    devices = {}
    for name, filename, metadata, declaration in variants:
        fixture = bytes.fromhex((ROOT / 'drivers/NativeAdreno418/Test' / filename).read_text())
        if len(fixture) != metadata['fixture_bytes'] or \
            hashlib.sha256(fixture).hexdigest() != metadata['fixture_sha256']:
            raise ValueError('independent compiled fixture changed: ' + name)
        encoded = re.search(rf'constexpr u8 {name}\[\] = \{{(.*?)\}};', code, re.S)
        if not encoded:
            raise ValueError('native profile is missing: ' + name)
        expected = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-f]+)', encoded[1]))
        if expected != device_bytes(fixture) or len(expected) != metadata['bytes']:
            raise ValueError('native profile differs from the independently compiled device: ' + name)
        devices[name] = expected
    observed = json.loads((ROOT / handset['evidence']).read_text())
    captured = device_bytes(bytes(36) + bytes.fromhex(observed['candidate_hex']))
    if captured != devices['kMmu3Handset'] or len(captured) != observed['device_bytes'] or \
        hashlib.sha256(captured).hexdigest() != observed['device_sha256'] or \
        observed['device_sha256'] != handset['device_sha256']:
        raise ValueError('handset profile differs from the complete captured MMU3 Device')
    dependency = bytes.fromhex('085f44455012060150455030')
    if devices['kMmu3'].count(dependency) != 1:
        raise ValueError('pinned dependency encoding is not unique')
    omitted = bytearray(devices['kMmu3'].replace(dependency, b''))
    omitted[2:4] = bytes.fromhex('4f0c')
    if omitted != captured:
        raise ValueError('handset profile changes more than the observed dependency and package length')
    header = (ROOT / 'drivers/NativeAdreno418/SecureSmmu.h').read_text()
    for name, value in {'kGraphicsDevice': 18, 'kCallAvailable': 0x02000601,
        'kRestoreConfiguration': 0x02000c02, 'kContextOffset': 0x8000, 'kContextCount': 2}.items():
        match = re.search(rf'constexpr u32 {name}\s*=\s*(0x[0-9a-f]+|[0-9]+);', header)
        if not match or int(match[1], 0) != value:
            raise ValueError('secure monitor/profile constant changed: ' + name)
    if args.iasl:
        with tempfile.TemporaryDirectory(prefix='lumia-mmu3-profile-') as temporary:
            directory = Path(temporary)
            for name, filename, metadata, declaration in variants:
                isolated = directory / (name + '.asl')
                isolated.write_text('DefinitionBlock ("", "DSDT", 2, "QCOMM ", "MSM8992 ", 7)\n{\n'
                    'External (_SB.PEP0, DeviceObj)\nScope (\\_SB)\n{\n' + declaration + '\n}\n}\n')
                subprocess.run([args.iasl, '-tc', str(isolated)], cwd=directory, check=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if device_bytes(isolated.with_suffix('.aml').read_bytes()) != devices[name]:
                    raise ValueError('independent IASL recompilation differs from native profile: ' + name)
    print(json.dumps({'source_hash_verified': True, 'compiled_fixture_verified': True,
        'independent_iasl_recompilation_passed': bool(args.iasl), 'device_bytes': len(devices['kMmu3']),
        'handset_device_bytes': len(captured), 'captured_handset_variant_verified': True,
        'secure_monitor_device': 18, 'handset_secure_setup_or_dma_observed': False}, indent=2))


if __name__ == '__main__':
    main()
