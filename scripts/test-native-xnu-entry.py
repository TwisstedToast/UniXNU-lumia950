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
import xml.etree.ElementTree as ET


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


def read_registers(connection):
    registers = bytes.fromhex(request(connection, "g").decode())
    if len(registers) < 268:
        raise ValueError("short AArch64 register set")
    values = {f"x{index}": int.from_bytes(registers[index * 8:index * 8 + 8], "little") for index in range(31)}
    values.update(sp=int.from_bytes(registers[248:256], "little"),
        pc=int.from_bytes(registers[256:264], "little"), cpsr=int.from_bytes(registers[264:268], "little"))
    return values


def read_system_registers(connection):
    result = {}
    number = 0
    visited = set()
    wanted = {"elr_el1", "esr_el1", "far_el1", "sctlr_el1", "tcr_el1", "cntfrq_el0"}

    def feature(name):
        nonlocal number
        if name in visited or len(visited) >= 8:
            return
        visited.add(name)
        content = bytearray()
        for offset in range(0, 131072, 4096):
            chunk = request(connection, f"qXfer:features:read:{name}:{offset:x},1000")
            if not chunk or chunk[:1] not in (b"m", b"l"):
                return
            content.extend(chunk[1:])
            if chunk[:1] == b"l":
                break
        else:
            raise ValueError("QEMU register description exceeds the bounded size")
        for node in ET.fromstring(content.replace(b"xi:include", b"include")).iter():
            kind = node.tag.rsplit("}", 1)[-1]
            if kind == "include":
                feature(node.attrib["href"])
            elif kind == "reg":
                number = int(node.attrib.get("regnum", str(number)), 0)
                name = node.attrib["name"].lower()
                if name in wanted:
                    data = request(connection, f"p{number:x}")
                    if data and not data.startswith(b"E"):
                        result[name] = hex(int.from_bytes(bytes.fromhex(data.decode()), "little"))
                number += 1

    feature("target.xml")
    return result


def run(args):
    original = args.kernel.read_bytes()
    lean = (args.esp / "EFI/UniXNU/Payloads/kernel.macho").read_bytes()
    STAGE.verify_strip(original, lean)
    names = symbols(original)
    required = ["_arm_vm_init", "_arm64_eventi", "_gPEClockFrequencyInfo"]
    if any(name not in names for name in required):
        raise ValueError("required real XNU symbols are missing")
    if args.crypto_post:
        STAGE.crypto_contract(original)
    if args.finish_iokit and "_PE_init_iokit" not in names:
        raise ValueError("real IOKit initialization symbol is missing")
    stops = {name: address for name, address in names.items() if
        name.startswith("___ubsan_handle_") or name in (
            "_arm_vm_init", "_panic", "_panic_with_options", "_fleh_synchronous", "_sleh_synchronous_sp1")}
    if args.crypto_post:
        stops["_source_corecrypto14_bootstrap"] = names["_source_corecrypto14_bootstrap"]
    if args.finish_iokit:
        stops["_PE_init_iokit"] = names["_PE_init_iokit"]
    output = args.output.resolve()
    if output.exists():
        raise ValueError("use a new output directory to preserve earlier evidence")
    output.mkdir(parents=True)
    report = {"schema": "lumia950-xnu-entry-emulation-v1", "emulator_only": True,
        "handset_boot_observed": False, "ios_userspace_booted": False, "gpu_frame_observed": False,
        "kernel_sha256": hashlib.sha256(original).hexdigest(), "arm_vm_init_entry_observed": False,
        "arm_vm_init_return_observed": False,
        "architectural_counter_hz": 19200000,
        "source_crypto_bootstrap_return_observed": False,
        "iokit_return_observed": False,
        "source_crypto_bootstrap_symbols_present": set(STAGE.contract(original)["source_crypto_symbols"]) == STAGE.CRYPTO_SYMBOLS}
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
        command = [args.qemu, "-machine", "virt,gic-version=2", "-cpu", "cortex-a53,cntfrq=19200000", "-smp", "1",
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
                    entries = {names["_arm_vm_init"]: "arm_vm_init"}
                    if args.crypto_post:
                        entries[names["_source_corecrypto14_bootstrap"]] = "source_crypto_bootstrap"
                    if args.finish_iokit:
                        entries[names["_PE_init_iokit"]] = "iokit"
                    returns = {}
                    report["milestones"] = []
                    for _ in range(8):
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise TimeoutError("native boot observation deadline expired")
                        connection.settimeout(remaining)
                        try:
                            report["gdb_stop_reply"] = request(connection, "c").decode()
                        except TimeoutError:
                            report["observation_timed_out"] = True
                            connection.settimeout(5)
                            connection.sendall(b"\x03")
                            report["gdb_stop_reply"] = receive(connection).decode()
                            values = read_registers(connection)
                            break
                        values = read_registers(connection)
                        current = values["pc"]
                        if current in entries:
                            milestone = entries.pop(current)
                            report[milestone + "_entry_observed"] = True
                            report["milestones"].append(milestone + "_entry")
                            if milestone == "arm_vm_init" and not args.finish_vm:
                                break
                            return_address = values["x30"]
                            report[milestone + "_return_target"] = hex(return_address)
                            returns[return_address] = milestone
                            if request(connection, f"z1,{current:x},4") != b"OK" or \
                                request(connection, f"Z1,{return_address:x},4") != b"OK":
                                raise ValueError("could not observe an actual caller return address")
                        elif current in returns:
                            milestone = returns.pop(current)
                            report[milestone + "_return_observed"] = True
                            report["milestones"].append(milestone + "_return")
                            if milestone == "source_crypto_bootstrap":
                                report["source_crypto_status"] = int.from_bytes(
                                    read_memory(connection, names["_source_corecrypto14_status"], 4), "little")
                            if request(connection, f"z1,{current:x},4") != b"OK":
                                raise ValueError("could not retire the return breakpoint")
                            if report["arm_vm_init_return_observed"] and \
                                (not args.crypto_post or report["source_crypto_bootstrap_return_observed"]) and \
                                (not args.finish_iokit or report["iokit_return_observed"]):
                                break
                        else:
                            break
                    report["registers"] = {name: hex(value) for name, value in values.items()}
                    report["system_registers"] = read_system_registers(connection)
                    report["stop_symbols"] = [name for name, address in stops.items() if address == values["pc"]]
                    report["wfe_event_index"] = int.from_bytes(read_memory(connection, names["_arm64_eventi"], 4), "little")
                    report["clock_info_bytes"] = read_memory(connection, names["_gPEClockFrequencyInfo"], 0x58).hex()
                    report["stack_bytes"] = read_memory(connection, values["sp"], 256).hex()
                    if args.crypto_post:
                        report["source_crypto_status"] = int.from_bytes(
                            read_memory(connection, names["_source_corecrypto14_status"], 4), "little")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if report.get("system_registers", {}).get("cntfrq_el0") != hex(report["architectural_counter_hz"]):
        raise ValueError("the emulated architectural counter does not match the Lumia device-tree frequency")
    if not report["arm_vm_init_entry_observed"] or report["wfe_event_index"] != 4 or \
        args.finish_vm and not report["arm_vm_init_return_observed"]:
        raise ValueError(f"early native kernel boot stopped before VM setup: {report.get('stop_symbols')}; see {output}")
    if args.crypto_post and (not report["source_crypto_bootstrap_return_observed"] or report.get("source_crypto_status") != 0):
        raise ValueError(f"real source-provider initialization did not complete successfully: {report.get('stop_symbols')}; see {output}")
    if args.finish_iokit and not report["iokit_return_observed"]:
        raise ValueError(f"real IOKit initialization did not return: {report.get('stop_symbols')}; see {output}")
    if args.finish_iokit and report["milestones"] != ["arm_vm_init_entry", "arm_vm_init_return",
        "source_crypto_bootstrap_entry", "source_crypto_bootstrap_return", "iokit_entry", "iokit_return"]:
        raise ValueError("IOKit must initialize after the actual successful source crypto bootstrap")
    milestone = "returned from arm_vm_init" if args.finish_vm else "reached arm_vm_init entry"
    if args.crypto_post:
        milestone += " and the real source-provider POST/bootstrap"
    if args.finish_iokit:
        milestone += " and PE_init_iokit"
    print(f"PASS: real source XNU passed clock/WFE/topology setup and {milestone}; {output}")
    print("EMULATION ONLY. Handset boot, iOS userspace and GPU rendering are not proven.")


def main():
    parser = argparse.ArgumentParser(description="Bounded real ARM64 XNU early-boot instruction test; never accepts a loader bar as kernel proof")
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--esp", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--timeout", default=90, type=float)
    parser.add_argument("--finish-vm", action="store_true", help="Also require a real return to the saved VM caller; stop on intervening faults")
    parser.add_argument("--crypto-post", action="store_true", help="After VM, observe the actual source provider return and KERN_SUCCESS status; no overrides")
    parser.add_argument("--finish-iokit", action="store_true", help="Also require actual IOKit initialization entry/return after the real crypto POST")
    args = parser.parse_args()
    if args.finish_iokit:
        args.crypto_post = True
    if args.crypto_post:
        args.finish_vm = True
    if not 1 <= args.timeout <= 300:
        parser.error("timeout must be 1..300 seconds")
    try:
        run(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
