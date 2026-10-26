#!/usr/bin/env python3
"""Fetch or verify the pinned A418 command-processor firmware, without touching a phone."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import urllib.request

BASE = "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/20251021/"
FILES = {
    "a420_pm4.fw": ("qcom/a420_pm4.fw", 9556,
        "52eced2d9a908651d3f34ad5f68fc5f17d2c46c00b20530bbf1bedd54e5d5512", 1),
    "a420_pfp.fw": ("qcom/a420_pfp.fw", 4292,
        "cca23b697d4a08d8146893985f8196960106a936de74d9a60d6e70cca79d46eb", 5),
    "LICENSE.qcom": ("LICENSE.qcom", 0,
        "be904cd28cb292b80cdb6cf412ab0d9159d431671e987ad433c1f62e0988a9bc", None),
}


def inspect(name: str, data: bytes) -> dict:
    remote, size, digest, version_index = FILES[name]
    if size and len(data) != size:
        raise ValueError(f"{name}: size {len(data)}, expected {size}")
    actual = hashlib.sha256(data).hexdigest()
    if actual != digest:
        raise ValueError(f"{name}: SHA-256 mismatch; refusing a different firmware image")
    result = {"name": name, "bytes": len(data), "sha256": actual, "url": BASE + remote}
    if version_index is not None:
        if len(data) % 4 or len(data) < (version_index + 1) * 4:
            raise ValueError(f"{name}: invalid word layout")
        if name == "a420_pm4.fw" and len(data) % 12 != 4:
            raise ValueError(f"{name}: PM4 instructions must be triples after the header")
        result["version"] = f"0x{struct.unpack_from('<I', data, version_index * 4)[0]:08x}"
        result["image_words"] = len(data) // 4 - 1
    return result


def write_atomic(destination: Path, data: bytes) -> None:
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temporary:
        temporary_name = Path(temporary.name)
        try:
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
        except BaseException:
            temporary_name.unlink(missing_ok=True)
            raise
    try:
        temporary_name.replace(destination)
    finally:
        temporary_name.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--fetch", action="store_true")
    source.add_argument("--source-dir", type=Path)
    parser.add_argument("--output", type=Path, default=Path("out/adreno-firmware"))
    arguments = parser.parse_args()
    try:
        contents = {}
        manifest = {"gpu": "Adreno 418", "firmware_ref": "20251021", "files": [],
            "selection_source": "https://github.com/LineageOS/android_kernel_lge_msm8992/blob/"
            "d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/gpu/msm/adreno-gpulist.h",
            "hardware_validated": False}
        for name, (remote, _, _, _) in FILES.items():
            if arguments.fetch:
                with urllib.request.urlopen(BASE + remote, timeout=45) as response:
                    data = response.read(131073)
            else:
                data = (arguments.source_dir / name).read_bytes()
            if len(data) > 131072:
                raise ValueError(f"{name}: download exceeds the size limit")
            manifest["files"].append(inspect(name, data))
            contents[name] = data
        arguments.output.mkdir(parents=True, exist_ok=True)
        for name, data in contents.items():
            destination = arguments.output / name
            if destination.exists() and destination.read_bytes() != data:
                raise ValueError(f"{destination}: refusing to replace an unrelated existing file")
        for name, data in contents.items():
            write_atomic(arguments.output / name, data)
        write_atomic(arguments.output / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Verified PM4, PFP, and license: {arguments.output.resolve()}")
    print("No phone files or hardware registers were changed. Firmware compatibility is not a hardware test.")


if __name__ == "__main__":
    main()
