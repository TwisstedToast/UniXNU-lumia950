#!/usr/bin/env python3
"""Boot the built EFI probe in QEMU and verify its report and return path."""

import argparse
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time
from probe_fat import read_files


def run_probe(arguments: argparse.Namespace) -> str:
    executable = shutil.which(arguments.qemu)
    if executable is None:
        raise ValueError(f"QEMU not found: {arguments.qemu}")
    esp = arguments.esp.resolve()
    if not (esp / "EFI/BOOT/BOOTAA64.EFI").is_file():
        raise ValueError("ESP tree has no EFI/BOOT/BOOTAA64.EFI")
    firmware = arguments.firmware.resolve()
    if not firmware.is_file():
        raise ValueError(f"firmware not found: {firmware}")
    writable = getattr(arguments, "writable_report_test", False)
    disk = (f"readonly=off,file={arguments.report_image}" if writable
            else f"readonly=on,file=fat:ro:{esp}")
    command = [
        executable, "-machine", f"virt,gic-version={arguments.gic_version}",
        "-cpu", "cortex-a57", "-smp", "1", "-m", "768",
        "-bios", str(firmware), "-display", "none", "-serial", "stdio",
        "-monitor", "none", "-nic", "none", "-no-reboot", "-device", "virtio-gpu-pci",
        "-drive", f"if=none,id=probe,format=raw,{disk}",
        "-device", "virtio-blk-pci,drive=probe",
    ]
    transcript = bytearray()
    sent_return = False
    deadline = time.monotonic() + arguments.timeout
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as process:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while time.monotonic() < deadline:
                    events = selector.select(min(1, max(0, deadline - time.monotonic())))
                    if not events:
                        if process.poll() is not None:
                            break
                        continue
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    transcript.extend(chunk)
                    if b"NATIVE MMIO GUARD PASSED" in transcript or b"NATIVE MMIO GUARD FAILED" in transcript:
                        break
                    completed = b"PROBE COMPLETE" in transcript or b"RPM HANDOFF REFUSED" in transcript
                    if completed and not sent_return:
                        process.stdin.write(b"\r")
                        process.stdin.flush()
                        sent_return = True
                    if b"Returning to firmware." in transcript:
                        break
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    decoded = transcript.decode("utf-8", errors="replace")
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", decoded).replace("\r", "")


def run_report_test(arguments: argparse.Namespace) -> str:
    original = arguments.esp.resolve()
    transcripts = []
    with tempfile.TemporaryDirectory(prefix="lumia-efi-report-") as temporary:
        arguments.esp = Path(temporary) / "esp"
        shutil.copytree(original, arguments.esp)
        arguments.report_image = Path(temporary) / "report.img"
        converter = shutil.which(arguments.qemu_img)
        if converter is None:
            raise ValueError(f"qemu-img not found: {arguments.qemu_img}")
        subprocess.run([converter, "convert", "-f", "raw", "-O", "raw",
                        f"fat:ro:{arguments.esp}", str(arguments.report_image)], check=True)
        baseline = read_files(arguments.report_image)
        reports = {}
        for attempt in range(2):
            transcript = run_probe(arguments)
            if "REPORT_STAGE=COMPLETE" not in transcript or "REPORT_EFI_STATUS=0x0000000000000000" not in transcript:
                raise ValueError("writable report save failed:\n" + transcript)
            files = read_files(arguments.report_image)
            current = {path.name: data for path, data in files.items()
                       if path.parent == Path("EFI/LumiaProbe") and
                       re.fullmatch(r"gpu-report-[0-9][0-9]\.txt", path.name)}
            if len(current) != len(reports) + 1 or any(current.get(name) != data for name, data in reports.items()):
                raise ValueError("report append/preservation check failed")
            new_name = next(name for name in current if name not in reports)
            beginning = transcript.index("LUMIA 950 | GPU PREFLIGHT v3")
            ending = transcript.index("GPU PROBE COMPLETE\n", beginning) + len("GPU PROBE COMPLETE\n")
            expected = transcript[beginning:ending].encode("ascii")
            if current[new_name].replace(b"\r\n", b"\n") != expected:
                raise ValueError(f"report file does not contain the firmware transcript: {new_name}, "
                                 f"{len(current[new_name])} bytes, prefix={current[new_name][:100]!r}\n" + transcript)
            reports = current
            transcripts.append(f"REPORT BOOT {attempt + 1}\n" + transcript)
        for relative, data in baseline.items():
            if files.get(relative) != data:
                raise ValueError(f"emulator changed an existing ESP file: {relative}")
        transcripts.append("Two real EFI file writes persisted; earlier reports and existing ESP files preserved.\n")
    arguments.esp = original
    return "\n".join(transcripts)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--qemu-img", default="qemu-img")
    parser.add_argument("--gic-version", choices=("2", "3"), default="2")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--application", choices=("probe", "gpu", "rpm", "native-gpu", "guard"), default="probe")
    parser.add_argument("--writable-report-test", action="store_true",
                        help="GPU only: boot twice on a private temporary copy and verify saved files")
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if arguments.writable_report_test and arguments.application != "gpu":
        parser.error("--writable-report-test requires --application gpu")
    try:
        transcript = run_report_test(arguments) if arguments.writable_report_test else run_probe(arguments)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")
    if arguments.log:
        arguments.log.parent.mkdir(parents=True, exist_ok=True)
        arguments.log.write_text(transcript)
    print(transcript)
    evidence = {
        "gpu": ["LUMIA 950 | GPU PREFLIGHT v3", "PLATFORM_UNSUPPORTED: MMIO reads skipped.",
                "GPU PROBE COMPLETE", "Returning to firmware."],
        "probe": ["LUMIA 950 | UEFI BRING-UP PROBE v1", "Disk writes: NONE",
                  "CurrentEL: EL1", "DarwinBoot EL1: OK", "GOP:",
                  "PROBE COMPLETE", "Returning to firmware."],
        "rpm": ["LUMIA 950 | NATIVE RPM HANDOFF v3",
                "PLATFORM_UNSUPPORTED: no SMEM, translation or MMIO access.",
                "RPM HANDOFF REFUSED - firmware retained.", "Returning to firmware."],
        "native-gpu": ["LUMIA 950 | NATIVE GPU POWER v3",
                       "PLATFORM_UNSUPPORTED: no SMEM, translation or MMIO access.",
                       "RPM HANDOFF REFUSED - firmware retained.", "Returning to firmware."],
        "guard": ["NATIVE MMIO GUARD PASSED"],
    }
    required = evidence[arguments.application]
    missing = [marker for marker in required if marker not in transcript]
    for label in ("Memory descriptors", "Conventional RAM MiB", "Readable filesystem handles"):
        if arguments.application != "probe":
            break
        match = re.search(rf"{re.escape(label)}: ([0-9]+)", transcript)
        if match is None or int(match.group(1)) == 0:
            missing.append(label + " > 0")
    if missing:
        parser.exit(1, "error: missing smoke-test evidence: " + ", ".join(missing) + "\n")
    if arguments.application == "gpu":
        if "PLATFORM_MSM8992" in transcript or "GPU_STATE=" in transcript or "SMEM_PROTOCOL=" in transcript:
            parser.exit(1, "error: QEMU must never access Lumia MMIO\n")
        print(f"QEMU GICv{arguments.gic_version}: unsupported-platform refusal and return passed")
        if not arguments.writable_report_test:
            if "REPORT_STAGE=CREATE_FILE" not in transcript or "REPORT_EFI_STATUS=0x8000000000000008" not in transcript:
                parser.exit(1, "error: read-only report volume was not explicitly refused\n")
            print("Read-only report volume correctly reports EFI_WRITE_PROTECTED at CREATE_FILE.")
    elif arguments.application in ("rpm", "native-gpu"):
        forbidden = ("RPM_CHANNEL=", "DEVICE_TRANSLATIONS=", "PREFLIGHT_SAVE_STAGE=",
                     "Attempting native RPM handoff", "RPM HANDOFF READY", "GPU POWER READY",
                     "GPU_GCD_STATUS", "GPU_MMSS_SMMU_SPMI_MAPPINGS")
        if any(marker in transcript for marker in forbidden):
            parser.exit(1, "error: unsupported QEMU platform entered the native RPM path\n")
        print(f"QEMU GICv{arguments.gic_version}: RPM platform refusal and firmware return passed")
    elif arguments.application == "guard":
        print(f"QEMU GICv{arguments.gic_version}: actual EL1 load/store abort recovery and continued valid access passed")
    else:
        print(f"QEMU GICv{arguments.gic_version}: EFI entry, GOP, memory map, input and return passed")
    print("This is emulator evidence, not a Lumia hardware or XNU boot result.")


if __name__ == "__main__":
    main()
