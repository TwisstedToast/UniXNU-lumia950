#!/usr/bin/env python3
import argparse
import datetime
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import time


SPEC = importlib.util.spec_from_file_location("native_install", Path(__file__).with_name("install-native-bringup.py"))
INSTALL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALL)
ROOT_FLAG = "mmos_load_methods.txt"
ROOT_FLAG_SHA256 = "ca727706c9ea565cc6fe8536c4fc14161e52d5569214fc57f16ba181b2cb3447"
KERNEL = "EFI/UniXNU/Payloads/kernel.macho"
MANIFEST = "EFI/UniXNU/native-bringup.json"
UPDATABLE = {KERNEL, KERNEL + ".adt", "EFI/UniXNU/Drivers/LumiaGpuPower.efi", MANIFEST}
METADATA_DIRECTORIES = {".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"}


def metadata(name):
    return name.startswith("._") or name == ".DS_Store" or name in METADATA_DIRECTORIES


def installed_state(args, previous):
    volume, whole = INSTALL.measured_phone(args.volume_uuid, args.partition_uuid)
    if INSTALL.digest((INSTALL.PHONE / ROOT_FLAG).read_bytes()) != ROOT_FLAG_SHA256:
        raise ValueError("existing USB boot flag changed; no update")
    for name, expected in INSTALL.CRITICAL.items():
        if name.casefold() == "efi/boot/bootaa64.efi":
            continue
        if INSTALL.digest((INSTALL.PHONE / name).read_bytes()) != expected:
            raise ValueError(f"existing boot state changed: {name}; no update")
    for name, data in previous.items():
        destination = INSTALL.phone_file(name)
        if destination.is_symlink() or destination.read_bytes() != data:
            raise ValueError(f"installed stage differs from the supplied previous stage: {name}")
    expected = {name.casefold() for name in previous if name.startswith("EFI/UniXNU/")}
    actual = set()
    for directory, directories, filenames in os.walk(INSTALL.PHONE / "EFI/UniXNU", followlinks=False):
        directories[:] = [name for name in directories if not metadata(name)]
        for name in directories + filenames:
            source = Path(directory) / name
            if source.is_symlink():
                raise ValueError("unexpected phone symlink; no update")
        for name in filenames:
            if not metadata(name):
                actual.add((Path(directory) / name).relative_to(INSTALL.PHONE).as_posix().casefold())
    if actual != expected:
        raise ValueError("unexpected existing native tree file set; no update")
    return volume, whole


def snapshot():
    result = {ROOT_FLAG: (INSTALL.PHONE / ROOT_FLAG).read_bytes()}
    excluded = []
    for directory, directories, filenames in os.walk(INSTALL.PHONE, followlinks=False):
        for name in directories + filenames:
            source = Path(directory) / name
            if metadata(name):
                excluded.append(source.relative_to(INSTALL.PHONE).as_posix())
            elif source.is_symlink():
                raise ValueError("unexpected phone symlink; no update")
        directories[:] = [name for name in directories if not metadata(name)]
        for name in sorted(filenames):
            source = Path(directory) / name
            relative = source.relative_to(INSTALL.PHONE).as_posix()
            if not metadata(name) and relative != ROOT_FLAG:
                result[relative] = source.read_bytes()
    return result, sorted(excluded)


def update(args):
    previous = INSTALL.stage_files(args.previous_stage.resolve())
    current = INSTALL.stage_files(args.stage.resolve())
    changed = {name: data for name, data in current.items() if data != previous[name]}
    if not changed or set(changed) - UPDATABLE:
        raise ValueError("update must change only experimental GPU/kernel payloads and their manifest")
    volume, whole = installed_state(args, previous)
    growth = sum(max(0, ((len(data) + 511) // 512 - (len(previous[name]) + 511) // 512) * 512)
        for name, data in changed.items())
    scratch = max((len(data) for name, data in changed.items() if name != KERNEL), default=0)
    reserve = 1024 * 1024
    if volume["FreeSpace"] < growth + scratch + reserve:
        raise ValueError("not enough EFIESP space for the verified update and a 1 MiB reserve")
    release_kernel = KERNEL in changed and len(changed[KERNEL]) + growth + reserve > volume["FreeSpace"]
    summary = {"apply": args.apply, "phone": volume["DeviceIdentifier"],
        "volume_uuid": volume["VolumeUUID"], "partition_uuid": volume["DiskUUID"],
        "changed_files": sorted(changed), "free_bytes": volume["FreeSpace"],
        "non_atomic_kernel_replacement_after_backup": release_kernel,
        "verification_scope": "all nonmetadata files; macOS sidecars/cache are neither opened nor modified",
        "boot_chain_replaced": False}
    if not args.apply:
        print(json.dumps(summary, indent=2))
        return
    backup_root = args.backup_root.resolve()
    if not backup_root.is_relative_to(INSTALL.BACKUP_VOLUME.resolve()) or \
        INSTALL.disk_info(INSTALL.BACKUP_VOLUME)["ParentWholeDisk"] == whole["DeviceIdentifier"]:
        raise ValueError("verified backup must go to the separate bludssd volume")
    baseline, excluded = snapshot()
    summary["excluded_metadata_paths"] = excluded
    archived = io.BytesIO()
    with tarfile.open(fileobj=archived, mode="w") as archive:
        for name, data in baseline.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    backup_root.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup = Path(tempfile.mkdtemp(prefix=f"native-update-{timestamp}-", dir=backup_root))
    INSTALL.write_verified(backup / "EFIESP.tar", archived.getvalue())
    INSTALL.write_verified(backup / "fallback-rescue.efi", previous["EFI/BOOT/BOOTAA64.EFI"])
    before = {name: {"bytes": len(data), "sha256": INSTALL.digest(data)} for name, data in baseline.items()}
    INSTALL.write_verified(backup / "before.json", (json.dumps({"summary": summary, "files": before}, indent=2) + "\n").encode())
    time.sleep(2)
    with tarfile.open(backup / "EFIESP.tar", "r:") as verified:
        if set(verified.getnames()) != set(baseline):
            raise ValueError("backup archive file set mismatch; phone remains untouched")
        for name, data in baseline.items():
            if verified.extractfile(name).read() != data:
                raise ValueError(f"backup archive verification failed: {name}; phone remains untouched")
    if (backup / "fallback-rescue.efi").read_bytes() != previous["EFI/BOOT/BOOTAA64.EFI"]:
        raise ValueError("fallback backup changed; phone remains untouched")
    installed_state(args, previous)
    for name, data in baseline.items():
        if (INSTALL.PHONE / name).read_bytes() != data:
            raise ValueError(f"phone file changed during backup: {name}; phone remains untouched")
    if release_kernel:
        INSTALL.phone_file(KERNEL).unlink()
    for name in sorted(changed, key=lambda name: (name == MANIFEST, name != KERNEL, name)):
        INSTALL.write_verified(INSTALL.phone_file(name), changed[name])
    os.sync()
    time.sleep(2)
    for name, data in current.items():
        if INSTALL.phone_file(name).read_bytes() != data:
            raise ValueError(f"updated payload readback mismatch: {name}; backup is {backup}")
    overwritten = {name.casefold() for name in changed}
    for name, data in baseline.items():
        if name.casefold() not in overwritten and (INSTALL.PHONE / name).read_bytes() != data:
            raise ValueError(f"untouched phone file changed: {name}; backup is {backup}")
    summary.update(backup=str(backup), verified_nonmetadata_files=len(baseline),
        original_other_nonmetadata_files_preserved=True)
    INSTALL.write_verified(backup / "updated.json", (json.dumps(summary, indent=2) + "\n").encode())
    print(json.dumps(summary, indent=2))
    print("Verified experimental payload update only. BCD, BootShim, UEFI and fallback preserved. Disk remains mounted.")


def main():
    parser = argparse.ArgumentParser(description="Update an exactly verified existing native stage; dry-run unless --apply")
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--previous-stage", required=True, type=Path)
    parser.add_argument("--volume-uuid", required=True)
    parser.add_argument("--partition-uuid", required=True)
    parser.add_argument("--backup-root", type=Path, default=Path("/Volumes/bludssd/Lumia950-backups"))
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    try:
        update(args)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
