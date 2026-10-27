import importlib.util
import json
from pathlib import Path
import struct
import unittest


SPEC = importlib.util.spec_from_file_location('oem_scm_profile', Path(__file__).with_name('check-oem-scm-profile.py'))
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


def image():
    blob = bytearray(272)
    blob[:2] = b'MZ'
    struct.pack_into('<I', blob, 60, 64)
    blob[64:68] = b'PE\0\0'
    struct.pack_into('<HH', blob, 68, 0xaa64, 1)
    struct.pack_into('<H', blob, 84, 32)
    struct.pack_into('<H', blob, 88, 0x20b)
    struct.pack_into('<Q', blob, 112, 0x140000000)
    struct.pack_into('<IIII', blob, 128, 16, 0x1000, 16, 256)
    struct.pack_into('<I', blob, 256, 0xd4000003)
    return blob


class ProfileTests(unittest.TestCase):
    def test_source_constants(self):
        evidence = json.loads((PROFILE.ROOT / 'docs/hardware/2026-10-27-oem-scm-transport.json').read_text())
        PROFILE.validate_source(evidence)
        for name in evidence['native_constants']:
            changed = json.loads(json.dumps(evidence))
            changed['native_constants'][name] ^= 1
            with self.assertRaises(ValueError):
                PROFILE.validate_source(changed)

    def test_arm64_word(self):
        self.assertEqual(PROFILE.pe_word(image(), 0x140001000), 0xd4000003)
        self.assertEqual(PROFILE.pe_word(image(), 0x14000100c), 0)

    def test_rejected_layouts(self):
        for size in (0, 2, 63, 87, 119, 159, 259):
            with self.assertRaises(ValueError):
                PROFILE.pe_word(image()[:size], 0x140001000)
        for address in (0, 0x13fffffff, 0x140000ffc, 0x140001001, 0x140001010):
            with self.assertRaises(ValueError):
                PROFILE.pe_word(image(), address)
        for offset in (0, 64, 68, 70, 84, 88):
            changed = image()
            changed[offset] ^= 0x80
            with self.assertRaises(ValueError):
                PROFILE.pe_word(changed, 0x140001000)
        duplicate = image()
        struct.pack_into('<H', duplicate, 70, 2)
        duplicate[160:200] = duplicate[120:160]
        with self.assertRaises(ValueError):
            PROFILE.pe_word(duplicate, 0x140001000)
        unbacked = image()
        struct.pack_into('<I', unbacked, 140, 270)
        with self.assertRaises(ValueError):
            PROFILE.pe_word(unbacked, 0x140001000)


if __name__ == '__main__':
    unittest.main()
