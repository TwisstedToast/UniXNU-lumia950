#!/usr/bin/env python3
"""Run the shared GICv2 core with asynchronous AArch64 exceptions, not XNU."""
import argparse
import pathlib
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--qemu", default=shutil.which("qemu-system-aarch64"))
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("out/gicv2-validation"))
    args = parser.parse_args()
    if not args.qemu or not args.image.is_file():
        parser.error("QEMU and the built test ELF are required")
    args.output.mkdir(parents=True, exist_ok=True)
    blocked_image = args.image.with_name("gicv2-test-blocked-pmr.elf")
    if not blocked_image.is_file():
        parser.error("build-gicv2-test.sh must also produce the blocked-PMR negative test")
    for secure, blocked in ((False, False), (True, False), (True, True)):
        name = ("secure-on" if secure else "secure-off") + ("-blocked-pmr" if blocked else "")
        image = blocked_image if blocked else args.image
        command = [args.qemu, "-M", "virt,gic-version=2,virtualization=on,secure=" + ("on" if secure else "off"),
                   "-cpu", "cortex-a53", "-accel", "tcg", "-m", "128M", "-smp", "2",
                   "-nographic", "-monitor", "none", "-nic", "none", "-no-reboot",
                   "-semihosting-config", "enable=on,target=native", "-kernel", str(image.resolve())]
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=15, check=False)
        except subprocess.TimeoutExpired as error:
            (args.output / (name + ".log")).write_bytes(error.stdout or b"")
            raise SystemExit(f"{name}: QEMU timeout; see {args.output}") from error
        (args.output / (name + ".log")).write_bytes(result.stdout)
        if blocked:
            if result.returncode != 1 or b"FAIL GICC enable" not in result.stdout or b"PASS" in result.stdout:
                sys.stdout.buffer.write(result.stdout)
                raise SystemExit("blocked secure PMR was not refused")
            print(f"{name}: inaccessible secure-owned priority mask is refused")
            continue
        expected = [b"GICV2 IRQ TEST EL=0x0000000000000001", b"PASS ASYNC TIMER=2 SGI=2 SPI=2 EOIMODE=0"]
        expected.append(b"SECURITY EXTENSIONS ON" if secure else b"SECURITY EXTENSIONS OFF")
        if secure:
            expected.append(b"SECURE SPI REFUSED")
        if result.returncode or b"FAIL" in result.stdout or not all(marker in result.stdout for marker in expected):
            sys.stdout.buffer.write(result.stdout)
            raise SystemExit(f"{name}: IRQ validation failed ({result.returncode})")
        print(f"{name}: asynchronous virtual timer, SGI, SPI, rearm and EOIR passed")
    print("QEMU core validation only; the XNU/IOKit wrapper and Lumia hardware are unverified.")


if __name__ == "__main__":
    main()
