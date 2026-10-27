#!/usr/bin/env python3
import argparse
import contextlib
import importlib.util
import io
import hmac
import json
from pathlib import Path
import shutil
import tempfile
import tarfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("native_install", Path(__file__).with_name("install-native-bringup.py"))
INSTALL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALL)
FIXTURE_SPEC = importlib.util.spec_from_file_location("native_fixture", Path(__file__).with_name("test-native-bringup.py"))
FIXTURE = importlib.util.module_from_spec(FIXTURE_SPEC)
FIXTURE_SPEC.loader.exec_module(FIXTURE)
UPDATE_SPEC = importlib.util.spec_from_file_location("native_update", Path(__file__).with_name("update-native-bringup.py"))
UPDATE = importlib.util.module_from_spec(UPDATE_SPEC)
UPDATE_SPEC.loader.exec_module(UPDATE)


class NativeInstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.phone = self.root / "phone"
        self.ssd = self.root / "ssd"
        self.stage = self.root / "stage"
        self.phone.mkdir()
        self.ssd.mkdir()
        self.stage.mkdir()
        self.critical = {}
        for name in INSTALL.CRITICAL:
            destination = self.phone / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            data = (name + " original").encode()
            destination.write_bytes(data)
            self.critical[name] = INSTALL.digest(data)
        existing = self.phone / "EFI/LumiaProbe/earlier-report.txt"
        existing.parent.mkdir()
        existing.write_bytes(b"real earlier report")
        self.before = self.contents(self.phone)
        self.manifest = {"schema": "lumia950-native-bringup-v1", "merged_kernel_qualification_bypassed": False,
            "ios_userspace_included": False, "source_crypto_integrity_verified": True, "files": {}}
        for name in sorted(INSTALL.FILES):
            data = b"menu" if name.endswith(".EFI") or name.endswith("UniXNU.efi") else name.encode()
            if name.endswith(".toml"):
                data = b'[boot]\ndefault = "exit"\n[[entry]]\nid="gpu"\n[[entry]]\nid="xnu"\n[[entry]]\nid="exit"\n'
            if name.endswith("kernel.macho"):
                data = bytes(FIXTURE.crypto_image())
            self.set_file(name, data)
        self.save_manifest()
        self.volume = {"VolumeUUID": "expected-volume", "DiskUUID": "expected-partition", "FilesystemType": "msdos",
            "MountPoint": str(self.phone), "TotalSize": 33276416, "Writable": True,
            "DeviceIdentifier": "disk9s35", "ParentWholeDisk": "disk9", "FreeSpace": 18 * 1024 * 1024}
        self.whole = {"TotalSize": 31268536320, "MediaName": "Phone MMC Stor", "BusProtocol": "USB",
            "Internal": False, "DeviceIdentifier": "disk9"}
        for replacement in [patch.object(INSTALL, "PHONE", self.phone), patch.object(INSTALL, "BACKUP_VOLUME", self.ssd),
            patch.object(INSTALL, "CRITICAL", self.critical), patch.object(INSTALL, "disk_info", self.disk_info),
            patch.object(INSTALL.time, "sleep", lambda _: None), patch.object(INSTALL.os, "sync", lambda: None),
            patch.object(UPDATE, "INSTALL", INSTALL)]:
            replacement.start()
            self.addCleanup(replacement.stop)
        self.args = argparse.Namespace(stage=self.stage, volume_uuid="expected-volume", partition_uuid="expected-partition",
            backup_root=self.ssd / "backups", apply=True)

    def set_file(self, name, data):
        destination = self.stage / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        self.manifest["files"][name] = {"bytes": len(data), "sha256": INSTALL.digest(data)}

    def save_manifest(self):
        (self.stage / "native-bringup.json").write_text(json.dumps(self.manifest))

    def disk_info(self, device):
        if str(device) == str(self.phone):
            return self.volume.copy()
        if str(device) == str(self.ssd):
            return {"ParentWholeDisk": "disk8"}
        self.assertEqual(str(device), "/dev/disk9")
        return self.whole.copy()

    def contents(self, root):
        return {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()}

    def run_install(self):
        with contextlib.redirect_stdout(io.StringIO()):
            INSTALL.install(self.args)

    def test_backup_then_menu_replacement_preserves_other_files(self):
        self.run_install()
        backup = next(self.args.backup_root.iterdir())
        with tarfile.open(backup / "EFIESP.tar", "r:") as archive:
            self.assertEqual({name: archive.extractfile(name).read() for name in archive.getnames()}, self.before)
        self.assertEqual((backup / "fallback-rescue.efi").read_bytes(), self.before["EFI/boot/BOOTAA64.EFI"])
        for name, data in self.before.items():
            if name.casefold() != "efi/boot/bootaa64.efi":
                self.assertEqual((self.phone / name).read_bytes(), data)
        self.assertEqual((self.phone / "EFI/boot/BOOTAA64.EFI").read_bytes(), b"menu")
        self.assertTrue(json.loads((backup / "installed.json").read_text())["all_original_other_files_preserved"])

    def test_dry_run_writes_nothing(self):
        self.args.apply = False
        self.run_install()
        self.assertEqual(self.contents(self.phone), self.before)
        self.assertFalse(self.args.backup_root.exists())

    def test_wrong_device_writes_nothing(self):
        for key, value in [("VolumeUUID", "different"), ("DiskUUID", "different"), ("Writable", False), ("TotalSize", 1024)]:
            with self.subTest(key=key), patch.dict(self.volume, {key: value}):
                with self.assertRaisesRegex(ValueError, "identified Lumia"):
                    self.run_install()
        self.assertEqual(self.contents(self.phone), self.before)

    def test_changed_stage_and_unsafe_menu_refused(self):
        name = "EFI/UniXNU/unixnu.toml"
        source = self.stage / name
        original = source.read_bytes()
        source.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.run_install()
        self.set_file(name, original.replace(b'"exit"', b'"xnu"', 1))
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "explicit choices"):
            self.run_install()
        self.assertEqual(self.contents(self.phone), self.before)

    def test_backup_failure_never_changes_phone(self):
        with patch.object(INSTALL, "write_verified", side_effect=OSError("backup unavailable")):
            with self.assertRaisesRegex(OSError, "backup unavailable"):
                self.run_install()
        self.assertEqual(self.contents(self.phone), self.before)

    def test_manifest_cannot_assert_integrity_for_an_unsealed_image(self):
        data = FIXTURE.crypto_image()
        data[1536] ^= 1
        self.set_file("EFI/UniXNU/Payloads/kernel.macho", bytes(data))
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "not sealed"):
            self.run_install()
        self.assertEqual(self.contents(self.phone), self.before)

    def test_payload_write_failure_keeps_old_fallback(self):
        original_writer = INSTALL.write_verified

        def fail_payload(destination, data):
            if destination.is_relative_to(self.phone):
                raise OSError("payload write failed")
            original_writer(destination, data)

        with patch.object(INSTALL, "write_verified", side_effect=fail_payload):
            with self.assertRaisesRegex(OSError, "payload write failed"):
                self.run_install()
        self.assertEqual((self.phone / "EFI/boot/BOOTAA64.EFI").read_bytes(), self.before["EFI/boot/BOOTAA64.EFI"])

    def test_replacing_an_existing_native_tree_refused(self):
        (self.phone / "EFI/UniXNU").mkdir()
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.run_install()

    def prepare_update(self):
        (self.phone / UPDATE.ROOT_FLAG).write_bytes(b"USB\r\n")
        self.run_install()
        self.args.previous_stage = self.root / "previous-stage"
        shutil.copytree(self.stage, self.args.previous_stage)
        self.before_update = self.contents(self.phone)
        self.set_file("EFI/UniXNU/Drivers/LumiaGpuPower.efi", b"new native GPU binary")
        kernel = FIXTURE.crypto_image()
        kernel[1032] ^= 1
        kernel[1536:1568] = hmac.digest(b"\0", kernel[1024:1280], "sha256")
        self.set_file(UPDATE.KERNEL, bytes(kernel))
        self.save_manifest()

    def run_update(self):
        with contextlib.redirect_stdout(io.StringIO()):
            UPDATE.update(self.args)

    def test_update_verifies_previous_stage_and_preserves_boot_chain(self):
        self.prepare_update()
        self.run_update()
        backup = next(self.args.backup_root.glob("native-update-*"))
        with tarfile.open(backup / "EFIESP.tar", "r:") as archive:
            self.assertEqual({name: archive.extractfile(name).read() for name in archive.getnames()}, self.before_update)
        self.assertEqual((self.phone / "EFI/boot/BOOTAA64.EFI").read_bytes(), self.before_update["EFI/boot/BOOTAA64.EFI"])
        for name, data in self.before_update.items():
            if name not in UPDATE.UPDATABLE:
                self.assertEqual((self.phone / name).read_bytes(), data)
        self.assertEqual((self.phone / UPDATE.KERNEL).read_bytes(), (self.stage / UPDATE.KERNEL).read_bytes())
        self.assertTrue(json.loads((backup / "updated.json").read_text())["original_other_nonmetadata_files_preserved"])

    def test_update_dry_run_and_changed_previous_payload_never_write(self):
        self.prepare_update()
        self.args.apply = False
        self.run_update()
        self.assertEqual(self.contents(self.phone), self.before_update)
        self.args.apply = True
        target = self.phone / "EFI/UniXNU/Drivers/LumiaGpuPower.efi"
        target.write_bytes(b"different handset payload")
        before = self.contents(self.phone)
        with self.assertRaisesRegex(ValueError, "differs from.*previous stage"):
            self.run_update()
        self.assertEqual(self.contents(self.phone), before)
        self.assertFalse(list(self.args.backup_root.glob("native-update-*")))

    def test_update_cannot_change_fallback_or_unexpected_native_files(self):
        self.prepare_update()
        unexpected = self.phone / "EFI/UniXNU/unexpected.efi"
        unexpected.write_bytes(b"untracked")
        with self.assertRaisesRegex(ValueError, "unexpected existing native tree"):
            self.run_update()
        unexpected.unlink()
        for name in ["EFI/BOOT/BOOTAA64.EFI", "EFI/UniXNU/UniXNU.efi"]:
            self.set_file(name, b"different boot chain")
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "only experimental"):
            self.run_update()
        self.assertEqual(self.contents(self.phone), self.before_update)

    def test_update_skips_macos_metadata_without_opening_it(self):
        self.prepare_update()
        sidecar = self.phone / "EFI/._UniXNU"
        sidecar.write_bytes(b"metadata alias must never be opened")
        original_reader = Path.read_bytes

        def refuse_metadata(source):
            if UPDATE.metadata(source.name):
                raise AssertionError("opened a macOS metadata sidecar")
            return original_reader(source)

        with patch.object(Path, "read_bytes", refuse_metadata):
            self.run_update()
        backup = next(self.args.backup_root.glob("native-update-*"))
        with tarfile.open(backup / "EFIESP.tar", "r:") as archive:
            self.assertNotIn("EFI/._UniXNU", archive.getnames())
        self.assertIn("EFI/._UniXNU", json.loads((backup / "updated.json").read_text())["excluded_metadata_paths"])
        self.assertEqual(sidecar.read_bytes(), b"metadata alias must never be opened")

    def test_update_backup_failure_keeps_every_phone_file(self):
        self.prepare_update()
        with patch.object(INSTALL, "write_verified", side_effect=OSError("backup unavailable")):
            with self.assertRaisesRegex(OSError, "backup unavailable"):
                self.run_update()
        self.assertEqual(self.contents(self.phone), self.before_update)

    def test_update_low_space_releases_only_the_backed_up_experimental_kernel(self):
        self.prepare_update()
        self.volume["FreeSpace"] = 1024 * 1024 + 2048
        original_writer = INSTALL.write_verified
        kernel_existed_at_write = []

        def record_kernel_write(destination, data):
            if destination == self.phone / UPDATE.KERNEL:
                kernel_existed_at_write.append(destination.exists())
            original_writer(destination, data)

        with patch.object(INSTALL, "write_verified", record_kernel_write):
            self.run_update()
        self.assertEqual(kernel_existed_at_write, [False])
        backup = next(self.args.backup_root.glob("native-update-*"))
        with tarfile.open(backup / "EFIESP.tar", "r:") as archive:
            self.assertEqual(archive.extractfile(UPDATE.KERNEL).read(), self.before_update[UPDATE.KERNEL])
        self.assertTrue(json.loads((backup / "updated.json").read_text())["non_atomic_kernel_replacement_after_backup"])
        self.assertEqual((self.phone / "EFI/boot/BOOTAA64.EFI").read_bytes(), self.before_update["EFI/boot/BOOTAA64.EFI"])

    def test_update_wrong_boot_flag_and_insufficient_space_refused(self):
        self.prepare_update()
        (self.phone / UPDATE.ROOT_FLAG).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "USB boot flag"):
            self.run_update()
        (self.phone / UPDATE.ROOT_FLAG).write_bytes(b"USB\r\n")
        self.volume["FreeSpace"] = 1024
        with self.assertRaisesRegex(ValueError, "not enough EFIESP"):
            self.run_update()
        self.assertEqual(self.contents(self.phone), self.before_update)


if __name__ == "__main__":
    unittest.main()
