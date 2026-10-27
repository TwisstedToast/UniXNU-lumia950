#!/usr/bin/env python3
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time


SPEC = importlib.util.spec_from_file_location("native_stage", Path(__file__).with_name("stage-native-bringup.py"))
STAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAGE)


def symbols(blob):
    STAGE.contract(blob)
    offset = 32
    result = {}
    for _ in range(struct.unpack_from("<I", blob, 16)[0]):
        command, length = struct.unpack_from("<II", blob, offset)
        if command == 2:
            table, count, strings, string_bytes = struct.unpack_from("<4I", blob, offset + 8)
            for index in range(count):
                name_offset, kind, section, _, value = struct.unpack_from("<IBBHQ", blob, table + index * 16)
                if not name_offset or kind & 0xE0 or (kind & 0xE) != 0xE or not section:
                    continue
                stop = blob.index(b"\0", strings + name_offset, strings + string_bytes)
                result[blob[strings + name_offset:stop].decode()] = value
        offset += length
    return result


def receive(connection):
    while True:
        byte = connection.recv(1)
        if not byte:
            raise ValueError("GDB connection closed before a reply")
        if byte == b"$":
            break
    reply = bytearray()
    while True:
        byte = connection.recv(1)
        if not byte:
            raise ValueError("truncated GDB reply")
        if byte == b"#":
            break
        reply.extend(byte)
    checksum = bytearray()
    while len(checksum) < 2:
        byte = connection.recv(2 - len(checksum))
        if not byte:
            raise ValueError("truncated GDB checksum")
        checksum.extend(byte)
    if int(checksum, 16) != sum(reply) % 256:
        raise ValueError("GDB checksum mismatch")
    connection.sendall(b"+")
    return bytes(reply)


def request(connection, command):
    packet = command.encode()
    connection.sendall(b"$" + packet + b"#" + f"{sum(packet) % 256:02x}".encode())
    reply = receive(connection)
    while reply.startswith(b"O") and reply != b"OK":
        reply = receive(connection)
    return reply


def read_memory(connection, address, length):
    response = request(connection, f"m{address:x},{length:x}")
    if response.startswith(b"E"):
        raise ValueError(f"GDB could not read {address:#x}: {response!r}")
    data = bytes.fromhex(response.decode())
    if len(data) != length:
        raise ValueError("short GDB memory read")
    return data


def run(args):
    original = args.kernel.read_bytes()
    lean = (args.esp / "EFI/UniXNU/Payloads/kernel.macho").read_bytes()
    STAGE.verify_strip(original, lean)
    names = symbols(original)
    required = ["_arm_vm_init", "_arm64_eventi", "_gPEClockFrequencyInfo"]
    if any(name not in names for name in required):
        raise ValueError("required real XNU symbols are missing")
    stops = {name: address for name, address in names.items() if
        name.startswith("___ubsan_handle_") or name in (
            "_arm_vm_init", "_panic", "_panic_with_options", "_fleh_synchronous", "_sleh_synchronous_sp1")}
    output = args.output.resolve()
    if output.exists():
        raise ValueError("use a new output directory to preserve earlier evidence")
    output.mkdir(parents=True)
    report = {"schema": "lumia950-xnu-entry-emulation-v1", "emulator_only": True,
        "handset_boot_observed": False, "ios_userspace_booted": False, "gpu_frame_observed": False,
        "kernel_sha256": hashlib.sha256(original).hexdigest(), "arm_vm_init_entry_observed": False}
    with tempfile.TemporaryDirectory(prefix="lumia-xnu-", dir="/tmp") as temporary:
        directory = Path(temporary)
        esp = directory / "esp"
        shutil.copytree(args.esp, esp)
        config = esp / "EFI/UniXNU/unixnu.toml"
        text, changed = re.subn(r'^default = "exit"$', 'default = "xnu"', config.read_text(), flags=re.M)
        text, timeout_changed = re.subn(r'^timeout = 300$', 'timeout = 1', text, flags=re.M)
        if changed != 1 or timeout_changed != 1:
            raise ValueError("expected the explicit non-autoboot bring-up menu")
        config.write_text(text)
        endpoint = directory / "gdb.sock"
        command = [args.qemu, "-machine", "virt,gic-version=2", "-cpu", "cortex-a53", "-smp", "1",
            "-m", "768", "-bios", str(args.firmware.resolve()), "-display", "none", "-serial", "stdio",
            "-monitor", "none", "-nic", "none", "-no-reboot", "-device", "ramfb", "-S",
            "-gdb", f"unix:{endpoint},server=on,wait=off",
            "-drive", f"if=none,id=boot,format=raw,readonly=on,file=fat:ro:{esp}",
            "-device", "virtio-blk-pci,drive=boot"]
        with (output / "serial.log").open("wb") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + args.timeout
                while not endpoint.exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise ValueError("QEMU GDB server did not start")
                    time.sleep(0.1)
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(max(1, deadline - time.monotonic()))
                    connection.connect(str(endpoint))
                    request(connection, "qSupported")
                    for address in sorted(set(stops.values())):
                        if request(connection, f"Z1,{address:x},4") != b"OK":
                            raise ValueError("QEMU refused a real instruction breakpoint")
                    report["gdb_stop_reply"] = request(connection, "c").decode()
                    registers = bytes.fromhex(request(connection, "g").decode())
                    if len(registers) < 268:
                        raise ValueError("short AArch64 register set")
                    values = {f"x{index}": int.from_bytes(registers[index * 8:index * 8 + 8], "little") for index in range(31)}
                    values.update(sp=int.from_bytes(registers[248:256], "little"),
                        pc=int.from_bytes(registers[256:264], "little"), cpsr=int.from_bytes(registers[264:268], "little"))
                    report["registers"] = {name: hex(value) for name, value in values.items()}
                    report["stop_symbols"] = [name for name, address in stops.items() if address == values["pc"]]
                    report["arm_vm_init_entry_observed"] = values["pc"] == names["_arm_vm_init"]
                    report["wfe_event_index"] = int.from_bytes(read_memory(connection, names["_arm64_eventi"], 4), "little")
                    report["clock_info_bytes"] = read_memory(connection, names["_gPEClockFrequencyInfo"], 0x58).hex()
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["arm_vm_init_entry_observed"] or report["wfe_event_index"] != 4:
        raise ValueError(f"early native kernel boot stopped before VM setup: {report.get('stop_symbols')}; see {output}")
    print(f"PASS: real source XNU passed clock/WFE/topology setup and reached arm_vm_init entry; {output}")
    print("EMULATION ONLY. VM initialization, handset boot, iOS userspace and GPU rendering are not proven.")


def main():
    parser = argparse.ArgumentParser(description="Bounded real ARM64 XNU early-boot instruction test; never accepts a loader bar as kernel proof")
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--esp", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--timeout", default=90, type=float)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 300:
        parser.error("timeout must be 1..300 seconds")
    try:
        run(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
