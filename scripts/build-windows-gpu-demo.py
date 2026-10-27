#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "tools" / "gpu-demo"


def prepare(output):
    output.mkdir(parents=True, exist_ok=True)
    shader = (SOURCES / "Animation.hlsl").read_text()
    if ')LUMIA_HLSL"' in shader:
        raise ValueError("shader conflicts with source delimiter")
    (output / "ShaderSource.h").write_text(
        'static const char kShaderSource[] = R"LUMIA_HLSL(\n' + shader + ')LUMIA_HLSL";\n'
    )
    shutil.copy2(SOURCES / "README.md", output / "README.txt")


def validate_image(path, architecture):
    image = path.read_bytes()
    if image[:2] != b"MZ":
        raise ValueError("not a DOS/PE executable")
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    machine = struct.unpack_from("<H", image, pe + 4)[0]
    magic = struct.unpack_from("<H", image, pe + 24)[0]
    subsystem = struct.unpack_from("<H", image, pe + 24 + 68)[0]
    expected = {"arm64": 0xAA64, "x64": 0x8664}[architecture]
    if (machine, magic, subsystem) != (expected, 0x20B, 2):
        raise ValueError(f"wrong PE contract: {machine:x}/{magic:x}/{subsystem}")
    return {"bytes": len(image), "sha256": hashlib.sha256(image).hexdigest(),
            "machine": f"0x{machine:04x}", "subsystem": "windows_gui"}


def main():
    parser = argparse.ArgumentParser(description="Build the optional Windows hardware GPU baseline, not an EFI/iOS driver")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--arch", choices=["arm64", "x64"], default="arm64")
    parser.add_argument("--compiler", choices=["zig", "msvc"], default="zig")
    parser.add_argument("--zig", default="zig")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    output = args.out.resolve()
    prepare(output)
    if args.prepare_only:
        return
    executable = output / "LumiaGpuDemo.exe"
    if args.compiler == "zig":
        target = {"arm64": "aarch64-windows-gnu", "x64": "x86_64-windows-gnu"}[args.arch]
        command = [args.zig, "c++", "-target", target, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-fms-extensions", "-fno-exceptions", "-fno-rtti", "-nostdlib++", "-municode", "-Wl,--subsystem,windows",
                   "-I", str(output), "-I", str(SOURCES), str(SOURCES / "WindowsGpuDemo.cpp"),
                   "-o", str(executable), "-ld3d11", "-ldxgi", "-ldxguid", "-ld3dcompiler_47",
                   "-luser32", "-lgdi32", "-lshell32"]
        environment = dict(os.environ)
        environment.setdefault("ZIG_GLOBAL_CACHE_DIR", str(output / ".zig-global"))
        environment.setdefault("ZIG_LOCAL_CACHE_DIR", str(output / ".zig-local"))
    else:
        command = ["cl", "/nologo", "/std:c++17", "/O2", "/W4", "/WX", "/MT", "/EHsc",
                   "/D_CRT_SECURE_NO_WARNINGS", "/DUNICODE", "/D_UNICODE", f"/I{output}", f"/I{SOURCES}",
                   str(SOURCES / "WindowsGpuDemo.cpp"), f"/Fe:{executable}", f"/Fo:{output / 'WindowsGpuDemo.obj'}",
                   "/link", "/SUBSYSTEM:WINDOWS", "d3d11.lib", "dxgi.lib", "dxguid.lib", "d3dcompiler.lib",
                   "user32.lib", "gdi32.lib", "shell32.lib"]
        environment = None
    subprocess.run(command, env=environment, check=True)
    metadata = {"schema": "lumia950-windows-gpu-baseline-v1", "architecture": args.arch,
                "native_ios_gpu_driver": False, "handset_executed": False,
                "hardware_fps_measured": False, "binary": validate_image(executable, args.arch),
                "sources": {name: hashlib.sha256((SOURCES / name).read_bytes()).hexdigest()
                            for name in ["WindowsGpuDemo.cpp", "Animation.hlsl", "Timing.h"]}}
    (output / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
