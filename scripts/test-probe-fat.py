#!/usr/bin/env python3
"""Check the read-only decoder used to verify real EFI report writes."""

from pathlib import Path
import struct
import tempfile
import unittest
from probe_fat import read_files


class FatTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="probe-fat-test-")
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "test.img"
        self.image = bytearray(4111 * 512)
        self.image[510:512] = b"\x55\xaa"
        struct.pack_into("<B3sB3sII", self.image, 446, 128, b"\0" * 3, 6, b"\0" * 3, 1, 4110)
        self.image[1022:1024] = b"\x55\xaa"
        struct.pack_into("<HBHBHHBH", self.image, 523, 512, 1, 1, 1, 32, 4110, 248, 17)
        struct.pack_into("<HHH", self.image, 1024, 0xfff8, 0xffff, 0xffff)
        self.root = 19 * 512
        self.data = 21 * 512
        self.image[self.root:self.root + 11] = b"FIRST   TXT"
        self.image[self.root + 11] = 32
        struct.pack_into("<HI", self.image, self.root + 26, 2, 7)
        self.image[self.data:self.data + 7] = b"report\n"

    def decode(self):
        self.path.write_bytes(self.image)
        return read_files(self.path)

    def test_short_name(self):
        self.assertEqual(self.decode(), {Path("FIRST.TXT"): b"report\n"})

    def test_long_name(self):
        entry = bytes(self.image[self.root:self.root + 32])
        checksum = 0
        for value in entry[:11]:
            checksum = ((((checksum & 1) << 7) | (checksum >> 1)) + value) & 255
        encoded = ("gpu-report-00.txt\0".encode("utf-16-le")).ljust(52, b"\xff")
        for index in range(2):
            offset = self.root + index * 32
            part = encoded[(1 - index) * 26:(2 - index) * 26]
            self.image[offset:offset + 32] = b"\0" * 32
            self.image[offset] = 66 if index == 0 else 1
            self.image[offset + 11] = 15
            self.image[offset + 13] = checksum
            self.image[offset + 1:offset + 11] = part[:10]
            self.image[offset + 14:offset + 26] = part[10:22]
            self.image[offset + 28:offset + 32] = part[22:]
        self.image[self.root + 64:self.root + 96] = entry
        self.assertEqual(self.decode(), {Path("gpu-report-00.txt"): b"report\n"})
        self.image[self.root + 13] ^= 1
        with self.assertRaisesRegex(ValueError, "long filename"):
            self.decode()

    def test_rejects_invalid_chains(self):
        for successor in (0, 1, 2, 4092, 0xfff7):
            with self.subTest(successor=successor):
                struct.pack_into("<H", self.image, 1028, successor)
                with self.assertRaisesRegex(ValueError, "cluster chain"):
                    self.decode()

    def test_rejects_short_payload(self):
        struct.pack_into("<I", self.image, self.root + 28, 513)
        with self.assertRaisesRegex(ValueError, "file size"):
            self.decode()

    def test_rejects_geometry(self):
        original = bytes(self.image)
        for offset, value in ((510, 0), (450, 0), (523, 1), (525, 3), (526, 0)):
            with self.subTest(offset=offset):
                self.image[:] = original
                self.image[offset] = value
                with self.assertRaises(ValueError):
                    self.decode()

    def test_rejects_partition_outside_file(self):
        self.image = self.image[:1024]
        with self.assertRaisesRegex(ValueError, "partition is outside"):
            self.decode()


if __name__ == "__main__":
    unittest.main()
