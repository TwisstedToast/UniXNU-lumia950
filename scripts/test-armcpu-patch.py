#!/usr/bin/env python3
"""Exercise qualified source preparation; does not compile the IOKit wrapper."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
import shutil
import subprocess
import sys

SPEC = importlib.util.spec_from_file_location("prepare_armcpu", Path(__file__).with_name("prepare-armcpu.py"))
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)
REFERENCE = b""


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "upstream/ArmCpu.cpp"
        self.source.parent.mkdir()
        self.source.write_bytes(REFERENCE)
        self.output = self.directory / "generated/ArmCpu.cpp"

    def test_prepares_without_mutating_upstream(self):
        manifest = PREPARE.prepare(self.source, self.output)
        self.assertEqual(self.source.read_bytes(), REFERENCE)
        self.assertEqual(hashlib.sha256(self.output.read_bytes()).hexdigest(), PREPARE.PATCHED_SHA256)
        self.assertEqual(manifest["generated_sha256"], PREPARE.PATCHED_SHA256)
        self.assertFalse(manifest["hardware_verified"])
        self.assertFalse(manifest["xnu_abi_compile_verified"])

    def test_repeat_is_deterministic(self):
        PREPARE.prepare(self.source, self.output)
        first = self.output.read_bytes()
        PREPARE.prepare(self.source, self.output)
        self.assertEqual(first, self.output.read_bytes())

    def test_modified_upstream_is_refused(self):
        self.source.write_bytes(REFERENCE + b"\n")
        with self.assertRaisesRegex(ValueError, "unqualified"):
            PREPARE.prepare(self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_source_overwrite_is_refused(self):
        with self.assertRaisesRegex(ValueError, "overwrite"):
            PREPARE.prepare(self.source, self.source)
        self.assertEqual(self.source.read_bytes(), REFERENCE)

    def test_modified_patch_is_refused(self):
        patch = self.directory / "bad.patch"
        original = (PREPARE.ROOT / "drivers/NativeGicV2/ArmCpu.patch").read_text()
        patch.write_text(original.replace("GICv2 CPU interface unavailable", "modified CPU interface"))
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            PREPARE.prepare(self.source, self.output, patch)
        self.assertFalse(self.output.exists())

    def test_assembled_tree_needs_no_ci_directory(self):
        tree = self.directory / "tree"
        script = tree / "scripts/prepare-armcpu.py"
        script.parent.mkdir(parents=True)
        shutil.copy2(Path(__file__).with_name("prepare-armcpu.py"), script)
        patch = tree / "drivers/NativeGicV2/ArmCpu.patch"
        patch.parent.mkdir(parents=True)
        shutil.copy2(PREPARE.ROOT / "drivers/NativeGicV2/ArmCpu.patch", patch)
        for name in ("GicV2Core.h", "GicV2Core.cpp"):
            shutil.copy2(PREPARE.ROOT / "drivers/NativeGicV2" / name, patch.parent / name)
        result = subprocess.run([sys.executable, str(script), str(self.source), str(self.output)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(hashlib.sha256(self.output.read_bytes()).hexdigest(), PREPARE.PATCHED_SHA256)


def main():
    global REFERENCE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    REFERENCE = args.source.read_bytes()
    if hashlib.sha256(REFERENCE).hexdigest() != PREPARE.SOURCE_SHA256:
        parser.error("expected the pinned upstream ArmCpu.cpp")
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(PreparationTests))
    raise SystemExit(not result.wasSuccessful())


if __name__ == "__main__":
    main()
