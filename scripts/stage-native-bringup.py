#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
NATIVE_KERNEL_SHA256 = "10c5edac166fe1da4e09e662856f73bcec1f00416a739e666c22854513844fc0"


def contract(blob):
    if len(blob) < 32:
        raise ValueError("short Mach-O")
    magic, cpu, subtype, kind, count, length = struct.unpack_from("<6I", blob)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x0100000C, 0, 2):
        raise ValueError("requires ARM64_ALL MH_EXECUTE, not arm64e or a container")
    if not count or count > 256 or not length or length > len(blob) - 32:
        raise ValueError("invalid Mach-O commands")
    offset, end = 32, 32 + length
    segments, symbols, entry = {}, {}, None
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError("truncated command")
        command, size = struct.unpack_from("<II", blob, offset)
        if size < 8 or size % 8 or offset + size > end:
            raise ValueError("invalid command extent")
        if command == 0x19:
            if size < 72:
                raise ValueError("short segment command")
            name = blob[offset + 8:offset + 24].split(b"\0")[0].decode("ascii")
            address, span, file_offset, file_size, maximum, protection, sections = struct.unpack_from("<QQQQIII", blob, offset + 24)
            if name in segments or sections > (size - 72) // 80:
                raise ValueError("duplicate segment or invalid section count")
            if file_size > span or file_offset > len(blob) or file_size > len(blob) - file_offset:
                raise ValueError("invalid segment file range")
            if address + span > (1 << 64) or address % 4096 or protection & ~maximum:
                raise ValueError("invalid virtual segment")
            if (name.startswith("__PRELINK") or name.startswith("__PLK_")) and (span or file_size):
                raise ValueError("merged/prelinked kernel refused; its runtime qualification gate is not bypassed")
            segments[name] = {"address": address, "span": span, "file_offset": file_offset,
                              "file_size": file_size, "protection": protection}
        elif command == 5:
            if entry is not None or size != 288 or struct.unpack_from("<II", blob, offset + 8) != (6, 68):
                raise ValueError("invalid ARM64 thread entry")
            entry = struct.unpack_from("<Q", blob, offset + 272)[0]
        elif command == 2:
            if size != 24:
                raise ValueError("invalid symbol command")
            table, entries, strings, string_bytes = struct.unpack_from("<4I", blob, offset + 8)
            if table + entries * 16 > len(blob) or strings + string_bytes > len(blob):
                raise ValueError("invalid symbol table")
            for index in range(entries):
                name_offset, symbol_type, section, description, value = struct.unpack_from("<IBBHQ", blob, table + index * 16)
                if symbol_type & 0xE0 or not name_offset:
                    continue
                if name_offset >= string_bytes:
                    raise ValueError("invalid symbol name")
                stop = blob.find(b"\0", strings + name_offset, strings + string_bytes)
                if stop < 0:
                    raise ValueError("unterminated symbol")
                name = blob[strings + name_offset:stop].decode("utf-8")
                if symbol_type & 0x0E == 0:
                    raise ValueError(f"unresolved native symbol: {name}")
                if symbol_type & 1:
                    if name in symbols and symbols[name] != value:
                        raise ValueError("ambiguous external symbol")
                    symbols[name] = value
        offset += size
    if offset != end or entry is None or entry & 3 or not segments or not symbols:
        raise ValueError("missing entry, segments or native symbol table")
    live = [value for value in segments.values() if value["span"]]
    for index, segment in enumerate(live):
        for previous in live[:index]:
            if segment["address"] < previous["address"] + previous["span"] and previous["address"] < segment["address"] + segment["span"]:
                raise ValueError("overlapping virtual segments")
    if not any(segment["protection"] & 4 and segment["address"] <= entry < segment["address"] + segment["file_size"] for segment in live):
        raise ValueError("entry is not file-backed executable code")
    base = min(segment["address"] for segment in live)
    span = max(segment["address"] + segment["span"] for segment in live) - base
    if span > 512 * 1024 * 1024 - 2 * 1024 * 1024:
        raise ValueError("native kernel exceeds the loader's owned window")
    return {"entry": entry, "base": base, "span": span, "header_bytes": end,
            "segments": segments, "external_symbols": symbols}


def verify_strip(original, stripped):
    before, after = contract(original), contract(stripped)
    if before["entry"] != after["entry"] or before["base"] != after["base"] or before["external_symbols"] != after["external_symbols"]:
        raise ValueError("stripping changed the entry, base or external symbol values")
    if before["segments"].keys() != after["segments"].keys():
        raise ValueError("stripping changed segment names")
    checked = 0
    for name, segment in before["segments"].items():
        if name == "__LINKEDIT":
            continue
        lean = after["segments"][name]
        for key in ["address", "span", "file_size", "protection"]:
            if segment[key] != lean[key]:
                raise ValueError(f"stripping changed {name} {key}")
        skip = before["header_bytes"] if segment["file_offset"] == 0 and segment["file_size"] else 0
        if skip != (after["header_bytes"] if lean["file_offset"] == 0 and lean["file_size"] else 0):
            raise ValueError("stripping changed load-command extent")
        size = segment["file_size"]
        if original[segment["file_offset"] + skip:segment["file_offset"] + size] != stripped[lean["file_offset"] + skip:lean["file_offset"] + size]:
            raise ValueError(f"stripping changed runtime bytes in {name}")
        checked += size - skip
    return checked, len(after["external_symbols"])


def menu_config():
    return '''version = 1
[boot]
timeout = 300
fallback_timeout = 10
connect_all = true
default = "exit"
[ui]
title = "Native Lumia 950"
logo = false
layout = "auto"
console = "auto"
[kits]
autoload = false
[[driver]]
path = "Drivers/DarwinBoot.efi"
[[entry]]
id = "gpu"
title = "Native Adreno hardware test"
type = "efi"
path = "Drivers/LumiaGpuPower.efi"
[[entry]]
id = "xnu"
title = "XNU kernel - no iOS userspace yet"
type = "xnu"
path = "Payloads/kernel.macho"
args = "-v keepsyms=1 debug=0x100 cpus=1"
[[entry]]
id = "exit"
title = "Exit to firmware"
type = "action"
action = "exit"
'''


def main():
    parser = argparse.ArgumentParser(description="Stage firmware-native GPU and source-only XNU bring-up; never installs or claims iOS")
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--kernel-sha256", default=NATIVE_KERNEL_SHA256)
    parser.add_argument("--menu-tree", required=True, type=Path)
    parser.add_argument("--gpu", required=True, type=Path)
    parser.add_argument("--strip", default="/usr/bin/strip")
    parser.add_argument("--unique-chip-id", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    original = args.kernel.read_bytes()
    if hashlib.sha256(original).hexdigest() != args.kernel_sha256:
        raise ValueError("native kernel does not match the explicit source-build hash")
    contract(original)
    if args.out.exists():
        raise ValueError("output must be a new directory, not an existing ESP or installation")
    with tempfile.TemporaryDirectory(prefix="lumia-native-kernel-") as temporary:
        lean = Path(temporary) / "kernel.macho"
        subprocess.run([args.strip, "-S", "-o", str(lean), str(args.kernel)], check=True)
        stripped = lean.read_bytes()
    checked, symbols = verify_strip(original, stripped)
    output = args.out.resolve()
    files = {
        "EFI/UniXNU/UniXNU.efi": (args.menu_tree / "EFI/UniXNU/UniXNU.efi").read_bytes(),
        "EFI/BOOT/BOOTAA64.EFI": (args.menu_tree / "EFI/UniXNU/UniXNU.efi").read_bytes(),
        "EFI/UniXNU/Drivers/DarwinBoot.efi": (args.menu_tree / "EFI/UniXNU/Drivers/DarwinBoot.efi").read_bytes(),
        "EFI/UniXNU/Drivers/LumiaGpuPower.efi": args.gpu.read_bytes(),
        "EFI/UniXNU/Payloads/kernel.macho": stripped,
        "EFI/UniXNU/unixnu.toml": menu_config().encode(),
    }
    for name, data in files.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    adt = output / "EFI/UniXNU/Payloads/kernel.macho.adt"
    subprocess.run([sys.executable, str(ROOT / "scripts/make-lumia950-adt.py"), "--output", str(adt),
        "--dram-size", "0xc0000000", "--dram-base", "0", "--owned-memory-size", "0x20000000",
        "--unique-chip-id", args.unique_chip_id, "--chip-id", "0x8015",
        "--cpu-regs", "0", "--psci-method", "none"], check=True)
    subprocess.run([sys.executable, str(ROOT / "scripts/check-adt.py"), str(adt)], check=True)
    subprocess.run([sys.executable, str(ROOT / "scripts/check-payload.py"), "--config",
        str(output / "EFI/UniXNU/unixnu.toml"), "--payload-dir", str(output), "--panel", "1080x1920"], check=True)
    manifest = {"schema": "lumia950-native-bringup-v1", "native_kernel_sha256": args.kernel_sha256,
        "stripped_kernel_sha256": hashlib.sha256(stripped).hexdigest(), "runtime_bytes_compared": checked,
        "external_symbol_values_preserved": symbols, "merged_kernel_qualification_bypassed": False,
        "root_filesystem_included": False, "ios_userspace_included": False, "handset_boot_observed": False,
        "exposed_boot_cpu_affinity": 0, "secondary_cpu_conduit_qualified": False,
        "gpu_frame_observed": False, "files": {}}
    for destination in sorted(output.rglob("*")):
        if destination.is_file():
            blob = destination.read_bytes()
            manifest["files"][destination.relative_to(output).as_posix()] = {
                "bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    (output / "native-bringup.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Staged {sum(value['bytes'] for value in manifest['files'].values())} bytes; no phone writes.")
    print("Source-only XNU early boot and native GPU hardware activation, NOT working iOS or a rendered frame.")


if __name__ == "__main__":
    main()
