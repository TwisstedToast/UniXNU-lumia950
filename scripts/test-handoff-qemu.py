#!/usr/bin/env python3
"""Exercise the EFI handoff test and inspect its post-EBS framebuffer in QEMU."""

import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import subprocess
import tempfile
import time


def ppm_pixels(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if header is None:
        raise ValueError(f"unsupported PPM header: {path}")
    width, height = (int(value) for value in header.groups())
    pixels = data[header.end():]
    if len(pixels) != width * height * 3:
        raise ValueError("PPM pixel data has the wrong length")
    return width, height, pixels


def qmp_command(stream, command: str, arguments: dict | None = None) -> dict:
    request = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\n")
    while True:
        line = stream.readline()
        if not line:
            raise ValueError("QMP connection closed")
        response = json.loads(line)
        if "error" in response:
            raise ValueError(f"QMP {command}: {response['error']}")
        if "return" in response:
            return response


def run_test(arguments: argparse.Namespace) -> None:
    executable = shutil.which(arguments.qemu)
    if executable is None:
        raise ValueError("qemu-system-aarch64 is unavailable")
    esp = arguments.esp.resolve()
    firmware = arguments.firmware.resolve()
    if not (esp / "EFI/BOOT/BOOTAA64.EFI").is_file() or not firmware.is_file():
        raise ValueError("ESP fallback application or firmware is missing")
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    transcript = bytearray()
    with tempfile.TemporaryDirectory(prefix="lumia-qmp-", dir="/tmp") as temporary:
        socket_path = str(Path(temporary) / "monitor.sock")
        command = [
            executable, "-machine", f"virt,gic-version={arguments.gic_version}",
            "-cpu", "cortex-a53", "-smp", "1", "-m", "768",
            "-bios", str(firmware), "-display", "none", "-serial", "stdio",
            "-monitor", "none", "-no-reboot", "-device", "ramfb",
            "-qmp", f"unix:{socket_path},server=on,wait=off",
            "-drive", f"if=none,id=probe,format=raw,readonly=on,file=fat:ro:{esp}",
            "-device", "virtio-blk-pci,drive=probe",
        ]
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT) as process:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)

                    def wait_for(marker: bytes) -> None:
                        deadline = time.monotonic() + 45
                        while marker not in transcript and time.monotonic() < deadline:
                            events = selector.select(0.5)
                            if not events:
                                if process.poll() is not None:
                                    break
                                continue
                            chunk = os.read(process.stdout.fileno(), 65536)
                            if not chunk:
                                break
                            transcript.extend(chunk)
                        if marker not in transcript:
                            raise ValueError(f"missing serial marker: {marker.decode()}")

                    wait_for(b"HANDOFF READY")
                    if arguments.cancel:
                        process.stdin.write(b"\r")
                        process.stdin.flush()
                        wait_for(b"HANDOFF CANCELLED - firmware retained.")
                        if b"Attempting ExitBootServices" in transcript:
                            raise ValueError("cancel path attempted ExitBootServices")
                        print("Cancel path: returns to firmware without ExitBootServices")
                        return
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                        connection.settimeout(10)
                        connection.connect(socket_path)
                        with connection.makefile("rwb", buffering=0) as stream:
                            greeting = json.loads(stream.readline())
                            if "QMP" not in greeting:
                                raise ValueError("invalid QMP greeting")
                            qmp_command(stream, "qmp_capabilities")
                            process.stdin.write(b"\x1b[A")
                            process.stdin.flush()
                            wait_for(b"Attempting ExitBootServices")
                            time.sleep(2)
                            first_path = output / "post-ebs-first.ppm"
                            second_path = output / "post-ebs-second.ppm"
                            qmp_command(stream, "screendump", {"filename": str(first_path)})
                            time.sleep(0.7)
                            qmp_command(stream, "screendump", {"filename": str(second_path)})
                            first_width, first_height, first = ppm_pixels(first_path)
                            second_width, second_height, second = ppm_pixels(second_path)
                            if (first_width, first_height) != (second_width, second_height):
                                raise ValueError("display geometry changed during the test")
                            if first[:3] != b"\x00\xff\x00" or second[:3] != b"\x00\xff\x00":
                                raise ValueError("post-EBS green marker did not reach the framebuffer")
                            if first == second:
                                raise ValueError("post-EBS heartbeat is not moving")
                            qmp_command(stream, "screendump", {
                                "filename": str(output / "post-ebs.png"), "format": "png"
                            })
                            print(f"QEMU GICv{arguments.gic_version}: green post-EBS marker and "
                                  f"moving heartbeat verified at {first_width}x{first_height}")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                text = transcript.decode("utf-8", errors="replace")
                (output / "serial.log").write_text(text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gic-version", choices=("2", "3"), default="2")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--cancel", action="store_true")
    arguments = parser.parse_args()
    try:
        run_test(arguments)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"error: {error}\n")
    print("This verifies the test application in QEMU, not the Lumia or XNU.")


if __name__ == "__main__":
    main()
