import importlib.util
from pathlib import Path
import struct
import unittest


spec = importlib.util.spec_from_file_location("native_stage", Path(__file__).with_name("stage-native-bringup.py"))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


def image():
    data = bytearray(4096)
    base = 0xFFFFFFF007004000
    struct.pack_into("<8I", data, 0, 0xFEEDFACF, 0x0100000C, 0, 2, 3, 464, 0, 0)
    struct.pack_into("<II16sQQQQIIII", data, 32, 0x19, 152, b"__TEXT", base, 4096, 0, 4096, 5, 5, 1, 0)
    struct.pack_into("<16s16sQQ8I", data, 104, b"__text", b"__TEXT", base + 512, 256, 512, 2, 0, 0, 0x80000400, 0, 0, 0)
    struct.pack_into("<4I", data, 184, 5, 288, 6, 68)
    struct.pack_into("<Q", data, 184 + 272, base + 512)
    struct.pack_into("<6I", data, 472, 2, 24, 2048, 1, 2064, 8)
    struct.pack_into("<IBBHQ", data, 2048, 1, 0x0F, 1, 0, base + 512)
    data[2064:2072] = b"\0_start\0"
    data[512:516] = bytes.fromhex("1f2003d5")
    return data


class NativeBringupTests(unittest.TestCase):
    def test_valid_source_kernel(self):
        data = image()
        result = stage.contract(data)
        self.assertEqual(result["entry"], 0xFFFFFFF007004200)
        self.assertEqual(len(result["external_symbols"]), 1)
        self.assertEqual(stage.verify_strip(data, data), (3600, 1))

    def test_invalid_native_contracts(self):
        for mutation in range(10):
            with self.subTest(mutation=mutation):
                data = image()
                if mutation == 0:
                    struct.pack_into("<I", data, 8, 2)
                elif mutation == 1:
                    struct.pack_into("<I", data, 12, 0xC)
                elif mutation == 2:
                    struct.pack_into("<I", data, 20, 5000)
                elif mutation == 3:
                    data[40:56] = b"__PRELINK_TEXT\0\0"
                elif mutation == 4:
                    struct.pack_into("<Q", data, 184 + 272, 0)
                elif mutation == 5:
                    data[2048 + 4] = 1
                elif mutation == 6:
                    struct.pack_into("<I", data, 472 + 12, 1000)
                elif mutation == 7:
                    struct.pack_into("<Q", data, 32 + 24, 3)
                elif mutation == 8:
                    struct.pack_into("<I", data, 32 + 60, 7)
                elif mutation == 9:
                    struct.pack_into("<Q", data, 32 + 48, 8192)
                with self.assertRaises(ValueError):
                    stage.contract(data)

    def test_text_change_never_qualifies_as_debug_stripping(self):
        original = image()
        changed = image()
        changed[516] ^= 1
        with self.assertRaisesRegex(ValueError, "runtime bytes"):
            stage.verify_strip(original, changed)

    def test_external_symbol_change_refused(self):
        original = image()
        changed = image()
        struct.pack_into("<Q", changed, 2048 + 8, 0xFFFFFFF007004204)
        with self.assertRaisesRegex(ValueError, "external symbol"):
            stage.verify_strip(original, changed)

    def test_menu_does_not_claim_ios_or_autoboot_a_test(self):
        import tomllib
        menu = tomllib.loads(stage.menu_config())
        self.assertEqual(menu["boot"]["default"], "exit")
        self.assertEqual(len(menu["entry"]), 3)
        self.assertIn("no iOS userspace", menu["entry"][1]["title"])


if __name__ == "__main__":
    unittest.main()
