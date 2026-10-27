#!/usr/bin/env python3
import argparse
import datetime
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import tarfile
import time
import tomllib


SPEC = importlib.util.spec_from_file_location("native_stage", Path(__file__).with_name("stage-native-bringup.py"))
STAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAGE)


PHONE = Path("/Volumes/EFIESP")
BACKUP_VOLUME = Path("/Volumes/bludssd")
FILES = {
    "EFI/BOOT/BOOTAA64.EFI", "EFI/UniXNU/UniXNU.efi", "EFI/UniXNU/Drivers/DarwinBoot.efi",
    "EFI/UniXNU/Drivers/LumiaGpuPower.efi", "EFI/UniXNU/Payloads/kernel.macho",
    "EFI/UniXNU/Payloads/kernel.macho.adt", "EFI/UniXNU/unixnu.toml",
}
CRITICAL = {
    "UEFI.elf": "00420de94f7a1ff3d2056c51ba856298cec10655c9840341114c518f19410c9f",
    "EFI/boot/BootShim.efi": "3b3c7a7f676c297b6035d63b2e3630e012d6223ab9ebb3ab36b76e9884531a3d",
    "EFI/boot/bootarm.efi": "e003475624d425723ac821a02bed50d0d740faed0552a652390c1dae43308509",
    "EFI/Microsoft/BOOT/BCD": "127c7154f1261b2cf8f6422222f80836b77655ede360edaec1c6318954f17759",
    "EFI/boot/BOOTAA64.EFI": "b782ee89bb670a62bf48de079554b94d43c22944ed24add723d93aac82ee34fc",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def phone_file(name):
    return PHONE / ("EFI/boot/BOOTAA64.EFI" if name.casefold() == "efi/boot/bootaa64.efi" else name)


def disk_info(device):
    return plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", str(device)]))


def phone_identity(volume_uuid, partition_uuid):
    volume = disk_info(PHONE)
    whole = disk_info("/dev/" + volume["ParentWholeDisk"])
    if volume.get("VolumeUUID") != volume_uuid or volume.get("DiskUUID") != partition_uuid or \
        volume.get("FilesystemType") != "msdos" or volume.get("MountPoint") != str(PHONE) or \
        volume.get("TotalSize") != 33276416 or not volume.get("Writable") or \
        whole.get("TotalSize") != 31268536320 or whole.get("MediaName") != "Phone MMC Stor" or \
        whole.get("BusProtocol") != "USB" or whole.get("Internal"):
        raise ValueError("connected device is not the explicitly identified Lumia EFIESP")
    for name, expected in CRITICAL.items():
        if digest((PHONE / name).read_bytes()) != expected:
            raise ValueError(f"existing boot state changed: {name}; no installation")
    if (PHONE / "EFI/UniXNU").exists():
        raise ValueError("EFI/UniXNU already exists; refuse to overwrite an earlier installation")
    return volume, whole


def stage_files(stage):
    manifest_data = (stage / "native-bringup.json").read_bytes()
    manifest = json.loads(manifest_data)
    if manifest.get("schema") != "lumia950-native-bringup-v1" or \
        manifest.get("merged_kernel_qualification_bypassed") is not False or \
        manifest.get("source_crypto_integrity_verified") is not True or \
        manifest.get("ios_userspace_included") is not False or set(manifest.get("files", {})) != FILES:
        raise ValueError("unexpected native bring-up contract or file set")
    result = {}
    for name, expected in manifest["files"].items():
        source = stage / name
        if source.is_symlink():
            raise ValueError(f"stage symlink refused: {name}")
        data = source.read_bytes()
        if len(data) != expected["bytes"] or digest(data) != expected["sha256"]:
            raise ValueError(f"stage digest mismatch: {name}")
        result[name] = data
    menu = tomllib.loads(result["EFI/UniXNU/unixnu.toml"].decode())
    if menu["boot"]["default"] != "exit" or [entry["id"] for entry in menu["entry"]] != ["gpu", "xnu", "exit"]:
        raise ValueError("tests must be explicit choices, never automatic boot targets")
    if result["EFI/BOOT/BOOTAA64.EFI"] != result["EFI/UniXNU/UniXNU.efi"]:
        raise ValueError("fallback and named menu differ")
    STAGE.crypto_contract(result["EFI/UniXNU/Payloads/kernel.macho"])
    result["EFI/UniXNU/native-bringup.json"] = manifest_data
    return result


def write_verified(destination, data):
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=destination.name + ".lumia-", dir=destination.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if digest(temporary.read_bytes()) != digest(data):
            raise ValueError(f"temporary write failed verification: {destination}")
        os.replace(temporary, destination)
        temporary = None
        if destination.read_bytes() != data:
            raise ValueError(f"installed file failed verification: {destination}")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def install(args):
    files = stage_files(args.stage.resolve())
    volume, whole = phone_identity(args.volume_uuid, args.partition_uuid)
    required = sum(len(data) for data in files.values()) + 1024 * 1024
    if volume["FreeSpace"] < required:
        raise ValueError("not enough EFIESP free space for this stage and a 1 MiB reserve")
    summary = {"apply": args.apply, "phone": volume["DeviceIdentifier"],
        "stage_bytes": required - 1024 * 1024, "free_bytes": volume["FreeSpace"],
        "volume_uuid": volume["VolumeUUID"], "partition_uuid": volume["DiskUUID"]}
    if not args.apply:
        print(json.dumps(summary, indent=2))
        return
    backup_root = args.backup_root.resolve()
    if not backup_root.is_relative_to(BACKUP_VOLUME.resolve()) or \
        disk_info(BACKUP_VOLUME)["ParentWholeDisk"] == whole["DeviceIdentifier"]:
        raise ValueError("verified backup must go to the separate bludssd volume")
    backup_root.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup = Path(tempfile.mkdtemp(prefix=f"native-bringup-{timestamp}-", dir=backup_root))
    baseline = {}
    archived = io.BytesIO()
    archive = tarfile.open(fileobj=archived, mode="w")
    for source in sorted(PHONE.rglob("*")):
        if source.is_symlink():
            raise ValueError(f"unexpected phone symlink: {source}")
        if source.is_file():
            name = source.relative_to(PHONE).as_posix()
            data = source.read_bytes()
            member = tarfile.TarInfo(name)
            member.size = len(data)
            member.mtime = int(source.stat().st_mtime)
            archive.addfile(member, io.BytesIO(data))
            baseline[name] = {"bytes": len(data), "sha256": digest(data)}
    archive.close()
    archive_data = archived.getvalue()
    write_verified(backup / "EFIESP.tar", archive_data)
    write_verified(backup / "fallback-rescue.efi", (PHONE / "EFI/boot/BOOTAA64.EFI").read_bytes())
    write_verified(backup / "before.json", (json.dumps({"summary": summary, "files": baseline}, indent=2) + "\n").encode())
    time.sleep(2)
    if (backup / "EFIESP.tar").read_bytes() != archive_data or \
        digest((backup / "fallback-rescue.efi").read_bytes()) != CRITICAL["EFI/boot/BOOTAA64.EFI"]:
        raise ValueError("SSD backup changed after writing; phone remains untouched")
    with tarfile.open(backup / "EFIESP.tar", "r:") as verified:
        if set(verified.getnames()) != set(baseline):
            raise ValueError("backup archive file set mismatch; phone remains untouched")
        for name, expected in baseline.items():
            if digest(verified.extractfile(name).read()) != expected["sha256"]:
                raise ValueError(f"backup archive verification failed: {name}; phone remains untouched")
    phone_identity(args.volume_uuid, args.partition_uuid)
    for name, data in files.items():
        if name != "EFI/BOOT/BOOTAA64.EFI":
            write_verified(phone_file(name), data)
    for name, data in files.items():
        if name != "EFI/BOOT/BOOTAA64.EFI" and phone_file(name).read_bytes() != data:
            raise ValueError("staged phone files changed; old fallback remains intact")
    write_verified(PHONE / "EFI/boot/BOOTAA64.EFI", files["EFI/BOOT/BOOTAA64.EFI"])
    os.sync()
    time.sleep(2)
    for name, data in files.items():
        if phone_file(name).read_bytes() != data:
            raise ValueError(f"final readback mismatch: {name}; backup is {backup}")
    for name, expected in baseline.items():
        if name.casefold() != "efi/boot/bootaa64.efi" and digest((PHONE / name).read_bytes()) != expected["sha256"]:
            raise ValueError(f"pre-existing file changed unexpectedly: {name}; backup is {backup}")
    summary.update(backup=str(backup), installed_files=len(files), all_original_other_files_preserved=True)
    write_verified(backup / "installed.json", (json.dumps(summary, indent=2) + "\n").encode())
    print(json.dumps(summary, indent=2))
    print("Verified FAT-file installation only. BCD, BootShim, UEFI, Windows and partitions preserved. Disk remains mounted.")


def main():
    parser = argparse.ArgumentParser(description="Reversible file-only installer for the measured Lumia; dry-run unless --apply")
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--volume-uuid", required=True)
    parser.add_argument("--partition-uuid", required=True)
    parser.add_argument("--backup-root", type=Path, default=Path("/Volumes/bludssd/Lumia950-backups"))
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    try:
        install(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
