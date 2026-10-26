#!/usr/bin/env python3
"""Offline regression tests of the GPU firmware asset verifier."""

import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("adreno_firmware", Path(__file__).with_name("adreno-firmware.py"))
FIRMWARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIRMWARE)


class FirmwareTests(unittest.TestCase):
    def test_real_images_are_pinned(self):
        self.assertEqual(FIRMWARE.FILES["a420_pm4.fw"][1], 9556)
        self.assertEqual(FIRMWARE.FILES["a420_pfp.fw"][1], 4292)
        self.assertEqual(FIRMWARE.FILES["a420_pm4.fw"][3], 1)
        self.assertEqual(FIRMWARE.FILES["a420_pfp.fw"][3], 5)
        self.assertIn("20251021", FIRMWARE.BASE)

    def test_hash_rejects_another_image(self):
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            FIRMWARE.inspect("a420_pm4.fw", bytes(9556))

    def test_size_rejects_truncation(self):
        with self.assertRaisesRegex(ValueError, "size"):
            FIRMWARE.inspect("a420_pfp.fw", bytes(4291))

    def test_zero_container_header_is_valid(self):
        data = struct.pack("<4I", 0, 0x4ff065, 2, 3)
        spec = ("test.fw", len(data), hashlib.sha256(data).hexdigest(), 1)
        with patch.dict(FIRMWARE.FILES, {"a420_pm4.fw": spec}):
            result = FIRMWARE.inspect("a420_pm4.fw", data)
        self.assertEqual(result["version"], "0x004ff065")
        self.assertEqual(result["image_words"], 3)

    def test_pm4_instruction_alignment(self):
        data = struct.pack("<3I", 0, 0x4ff065, 2)
        spec = ("test.fw", len(data), hashlib.sha256(data).hexdigest(), 1)
        with patch.dict(FIRMWARE.FILES, {"a420_pm4.fw": spec}):
            with self.assertRaisesRegex(ValueError, "triples"):
                FIRMWARE.inspect("a420_pm4.fw", data)

    def test_pfp_version_is_word_five(self):
        data = struct.pack("<6I", 0, 1, 2, 3, 4, 0x4ff083)
        spec = ("test.fw", len(data), hashlib.sha256(data).hexdigest(), 5)
        with patch.dict(FIRMWARE.FILES, {"a420_pfp.fw": spec}):
            result = FIRMWARE.inspect("a420_pfp.fw", data)
        self.assertEqual(result["version"], "0x004ff083")

    def test_atomic_write_and_replace(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "asset.fw"
            FIRMWARE.write_atomic(destination, b"first")
            FIRMWARE.write_atomic(destination, b"second")
            self.assertEqual(destination.read_bytes(), b"second")
            self.assertEqual(len(list(Path(temporary).iterdir())), 1)


if __name__ == "__main__":
    unittest.main()
