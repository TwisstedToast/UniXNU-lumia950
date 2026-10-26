#!/usr/bin/env python3
"""Generate the MSM8992 Apple device tree (ADT) for the Lumia 950.

This is the board script named by `[payload.adt] tool` in board.toml. It writes
the sidecar DarwinBoot copies next to the kernel as `kernel.macho.adt` and the
tree ArmDTPlatform parses.

Two things are borrowed rather than invented:

  * The node encoding is Apple's, byte for byte the one in
    `scripts/qemu-entry/make-dt.py`: u32 property count, u32 child count, then
    for each property a 32-byte NUL-padded name, a u32 length and the value
    padded to four bytes, then the children.
  * The cell layout is that same fixture's: two address cells and two size
    cells at the root and under `arm-io`, with each address and size stored as
    a native-endian u64. That fixture is the only ADT this project has watched
    a real XNU platform expert accept (`scripts/test-armdt-platform.py`), so it
    is the layout to follow until a kernel here says otherwise.

What the tree says about the hardware is taken from the register map in
README.md and from the MSM8992 descriptions used by mainline Linux. What is
*not* known is recorded rather than papered over:

  * `intc` describes the real GICv2 (distributor `0xf9000000`, CPU interface
    `0xf9002000`). The hash-qualified ArmCpu GICv2 integration is now written,
    with standalone QEMU IRQ tests. XNU and handset validation remain pending.
  * `psci` says `arm,psci-1.0` because that is the compatible XNU's ARM
    platform expects, but MSM8992 implements a subset over HVC with no
    SYSTEM_OFF and no SYSTEM_RESET. See README.md, blocker 4.
  * The display node matches NativeMdss's register-identity driver, not a
    framebuffer provider or a working iOS display.
  * The GPU node is emitted because a driver now reads it (`NativeAdreno418`,
    rung 1). Its window and the MMSS window beside it are from Qualcomm's
    msm-3.10 device tree, not from mainline: see drivers/NativeAdreno418/
    BoardConstants.h for the sources of every value.
  * `reg` windows are described, but whether the firmware left them where the
    SoC map says is a question for the driver, not for this file.

The tree is deterministic except for `random-seed`, which is fresh on every run
(the payload contract wants that).
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# MSM8992 (Snapdragon 808) constants. README.md, "The ADT node" and the
# register table, carries the sources for the SDHCI windows.
DRAM_BASE = 0
TIMEBASE_FREQUENCY = 19_200_000
GIC_DISTRIBUTOR = (0xF9000000, 0x1000)
GIC_CPU_INTERFACE = (0xF9002000, 0x1000)
SDHCI_WINDOWS = ((0xF9824900, 0x1A0), (0xF9824000, 0x800))
SDHCI_INTERRUPTS = (123, 138)
# The GPU. Qualcomm's msm-3.10 device tree for this SoC (msm8992-gpu.dtsi and
# msm-gdsc.dtsi) is the source: kgsl-3d0@0xfdb00000, GIC SPI 33, and the OxiLi
# power domains in the MMSS clock controller at 0xfd8c0000. The driver maps both
# windows, so both are reg entries here.
KGSL_WINDOWS = ((0xFDB00000, 0x40000), (0xFD8C0000, 0x10000))
KGSL_INTERRUPT = 33
GPU_CHIP_ID = 0x04010800
# The display. Same tree: msm8992-mdss.dtsi gives mdp_phys, the DSI0 windows and
# mmss_misc_phys; gdsc_mdss lives in the MMSS clock controller at 0xfd8c2304,
# which is why the second window is 64 KiB and not the 4 KiB of the GPU node.
# The driver maps all five by index, so the order here is the driver's order.
MDSS_WINDOWS = (
    (0xFD900000, 0x90000),   # mdp_phys
    (0xFD8C0000, 0x10000),   # MMSS clock controller (gdsc_mdss)
    (0xFD994000, 0x260),     # dsi_ctrl
    (0xFD994500, 0x280),     # dsi_phy
    (0xFD828000, 0x108),     # mmss_misc_phys
)
MDSS_INTERRUPT = 83
# MPIDR affinities from Lumia950XLPkg's MSM8992 MADT: four A53s, then two A57s.
DEFAULT_CPU_REGS = "0,1,2,3,0x100,0x101"
CPU_COMPATIBLES = {
    0: "arm,cortex-a53", 1: "arm,cortex-a53", 2: "arm,cortex-a53", 3: "arm,cortex-a53",
    0x100: "arm,cortex-a57", 0x101: "arm,cortex-a57",
}
NVRAM_BANK_SIZE = 8192
NVRAM_COMMON_DATA = 2048

CHRP_SIGNATURE = 0x70
CHRP_BLOCK = 0x10

OSENVIRONMENTS = ("main", "recoveryos", "ramdisk")
CRYPTO_HASH_METHODS = ("sha2-384", "sha1")


def node(name: str, properties=(), children=()) -> bytes:
    """Encode one Apple device tree node."""
    if not 0 < len(name.encode()) < 32:
        raise ValueError(f"node name {name!r} does not fit the 32-byte field")
    encoded = [("name", name.encode() + b"\0"), *properties]
    seen = set()
    for key, _ in encoded:
        if len(key.encode()) >= 32:
            raise ValueError(f"property name {key!r} does not fit the 32-byte field")
        if key in seen:
            raise ValueError(f"duplicate property {key!r} on node {name!r}")
        seen.add(key)
    result = struct.pack("<II", len(encoded), len(children))
    for key, data in encoded:
        result += struct.pack("<32sI", key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b"".join(children)


def u32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def u64(value: int) -> bytes:
    return struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)


def string(*values: str) -> bytes:
    """Apple property holding one NUL-terminated string per value."""
    return b"".join(value.encode() + b"\0" for value in values)


def chrp_header(name: bytes, size: int) -> bytearray:
    """A CHRP NVRAM header whose length covers `size` bytes in 16-byte blocks."""
    if size % CHRP_BLOCK:
        raise ValueError("CHRP partitions are a whole number of 16-byte blocks")
    raw = bytearray(struct.pack("<BBH12s", CHRP_SIGNATURE, 0, size // CHRP_BLOCK, name))
    checksum = sum(raw)
    while checksum > 0xFF:
        checksum = (checksum & 0xFF) + (checksum >> 8)
    raw[1] = checksum
    return raw


def empty_nvram_proxy() -> list[tuple[str, bytes]]:
    """A volatile CHRP bank: no controller, nothing persisted.

    The layout is XNU's (iokit/Kernel/IONVRAMCHRPHandler.cpp): a 16-byte
    `nvram` header, the 16-byte Apple header (Adler-32 over everything from the
    generation field, then a generation and padding), a `common` partition with
    2 KiB of data, and a `free` partition filling the rest of the bank.
    """
    image = bytearray(NVRAM_BANK_SIZE)
    image[:16] = chrp_header(b"nvram", 32)
    image[32:48] = chrp_header(b"common", 16 + NVRAM_COMMON_DATA)
    free_at = 32 + 16 + NVRAM_COMMON_DATA
    image[free_at:free_at + 16] = chrp_header(b"free", len(image) - free_at)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]) & 0xFFFFFFFF)
    return [
        ("nvram-bank-size", u32(len(image))),
        ("nvram-bank-count", u32(1)),
        ("nvram-current-bank", u32(0)),
        ("nvram-proxy-data", bytes(image)),
    ]


def parse_cpu_regs(value: str) -> list[int]:
    try:
        regs = [int(item, 0) for item in value.split(",") if item.strip()]
    except ValueError as error:
        raise SystemExit(f"--cpu-regs expects comma-separated integers: {error}") from None
    if not regs:
        raise SystemExit("--cpu-regs needs at least one affinity value")
    if len(regs) != len(set(regs)):
        raise SystemExit("--cpu-regs repeats an affinity value")
    if any(not 0 <= reg < 1 << 24 for reg in regs):
        raise SystemExit("--cpu-regs values must fit the 32-bit reg property")
    return regs


def cpu_node(index: int, reg: int) -> bytes:
    if reg not in CPU_COMPATIBLES:
        raise ValueError(f"unsupported MSM8992 CPU affinity {reg:#x}")
    return node(
        f"cpu{index}",
        [
            ("state", string("running" if index == 0 else "waiting")),
            ("reg", u32(reg)),
            ("device_type", string("cpu")),
            ("compatible", string(CPU_COMPATIBLES[reg])),
            ("timebase-frequency", u32(TIMEBASE_FREQUENCY)),
        ],
    )


def build_tree(
    *,
    dram_size: int,
    dram_base: int = DRAM_BASE,
    cpu_regs: list[int] | None = None,
    unique_chip_id: int | None = None,
    chip_id: int | None = None,
    owned_memory_size: int | None = None,
    root_match_volume_uuid: uuid.UUID | None = None,
    psci_method: str = "hvc",
    osenvironment: str = "main",
    crypto_hash_method: str = "sha2-384",
    data_ramdisk: bool = False,
    packed_data_ramdisk: bool = False,
    trust_cache: bool = False,
    persistent_data: bool = False,
    text_fixups: tuple[str, ...] = (),
    seed: bytes | None = None,
) -> bytes:
    """Encode the whole tree. Every input is explicit so a test can pin it."""
    if osenvironment not in OSENVIRONMENTS:
        raise SystemExit(f"--osenvironment must be one of {', '.join(OSENVIRONMENTS)}")
    if crypto_hash_method not in CRYPTO_HASH_METHODS:
        raise SystemExit(f"--crypto-hash-method must be one of {', '.join(CRYPTO_HASH_METHODS)}")
    if data_ramdisk and packed_data_ramdisk:
        raise SystemExit("--data-ramdisk and --packed-data-ramdisk are alternatives")
    if persistent_data and not (data_ramdisk or packed_data_ramdisk):
        raise SystemExit("--persistent-data describes a Data seed; pass --data-ramdisk or --packed-data-ramdisk")
    regs = cpu_regs if cpu_regs is not None else parse_cpu_regs(DEFAULT_CPU_REGS)

    chosen: list[tuple[str, bytes]] = [
        ("dram-base", u64(dram_base)),
        ("dram-size", u64(dram_size)),
        ("random-seed", seed if seed is not None else os.urandom(64)),
        ("osenvironment", string(osenvironment)),
        ("crypto-hash-method", string(crypto_hash_method)),
    ]
    if chip_id is not None:
        chosen.append(("chip-id", u32(chip_id)))
    if unique_chip_id is not None:
        chosen.append(("unique-chip-id", u64(unique_chip_id)))
    chosen.extend(empty_nvram_proxy())
    if owned_memory_size is not None:
        chosen.append(("darwinboot-memory-size", u64(owned_memory_size)))
    if root_match_volume_uuid is not None:
        xml = (
            "<dict><key>IOProviderClass</key><string>IOMedia</string>"
            "<key>IOPropertyMatch</key><dict><key>UUID</key><string>"
            + str(root_match_volume_uuid).upper()
            + "</string></dict></dict>\0"
        )
        chosen.append(("root-matching", xml.encode()))
    if (data_ramdisk or packed_data_ramdisk) and not persistent_data:
        chosen.append(("ephemeral-storage", u32(1)))
    if text_fixups:
        chosen.append(("text-fixups", b"".join(item.encode() + b"\0" for item in text_fixups)))

    placeholders: list[tuple[str, bytes]] = []
    if data_ramdisk:
        placeholders.append(("RAMDisk", bytes(16)))
    if packed_data_ramdisk:
        placeholders.append(("PackedRAMDisk", bytes(16)))
    if trust_cache:
        placeholders.append(("TrustCache", bytes(16)))
    chosen_children = [node("memory-map", placeholders)] if placeholders else []

    intc = node(
        "intc",
        [
            ("compatible", string("qcom,msm-qgic2", "arm,gic-2")),
            ("interrupt-controller", b""),
            ("#interrupt-cells", u32(3)),
            ("reg", b"".join(u64(address) + u64(size) for address, size in (GIC_DISTRIBUTOR, GIC_CPU_INTERFACE))),
        ],
    )
    sdhci = node(
        "sdhci@f9824900",
        [
            ("compatible", string("qcom,msm8994-sdhci", "qcom,sdhci-msm-v4")),
            ("reg", b"".join(u64(address) + u64(size) for address, size in SDHCI_WINDOWS)),
            # Apple's three-cell interrupt encoding, as README.md records it.
            ("interrupts", b"".join(u32(0) + u32(irq) + u32(4) for irq in SDHCI_INTERRUPTS)),
            ("clock-frequency", u32(TIMEBASE_FREQUENCY)),
        ],
    )
    kgsl = node(
        "kgsl@fdb00000",
        [
            ("compatible", string("qcom,kgsl-3d0", "qcom,kgsl-3d")),
            ("reg", b"".join(u64(address) + u64(size) for address, size in KGSL_WINDOWS)),
            ("interrupts", u32(0) + u32(KGSL_INTERRUPT) + u32(4)),
            # The device tree's qcom,chipid: generation 4 (a4xx) is what the
            # driver's register database and command stream cover.
            ("chip-id", u32(GPU_CHIP_ID)),
        ],
    )
    mdss = node(
        "mdss@fd900000",
        [
            ("compatible", string("qcom,mdss_mdp", "qcom,mdp")),
            ("reg", b"".join(u64(address) + u64(size) for address, size in MDSS_WINDOWS)),
            ("interrupts", u32(0) + u32(MDSS_INTERRUPT) + u32(4)),
            # The panel the firmware's GOP mode describes, so a driver that has
            # the ADT but not the firmware still knows what it is driving.
            ("panel-width", u32(1440)),
            ("panel-height", u32(2560)),
        ],
    )
    arm_io = node(
        "arm-io",
        [
            ("device_type", string("arm-io")),
            ("#address-cells", u32(2)),
            ("#size-cells", u32(2)),
            # Identity window over the whole 4 GiB peripheral space. Children
            # use absolute SoC addresses, so an offset is the address.
            ("ranges", u64(0) + u64(0) + u64(1 << 32)),
        ],
        [intc, sdhci, kgsl, mdss],
    )

    children = [
        node("chosen", chosen, chosen_children),
        node("cpus", children=[cpu_node(index, reg) for index, reg in enumerate(regs)]),
    ]
    if len(regs) > 1 and psci_method != "none":
        children.append(node("psci", [("compatible", string("arm,psci-1.0")), ("method", string(psci_method))]))
    children += [
        node("defaults", [("sleep-disabled", u32(1))]),
        node("options"),
        node(
            "product",
            [
                ("product-name", string("Lumia 950")),
                ("model", string("microsoft,lumia950")),
            ],
        ),
        arm_io,
    ]
    return node(
        "device-tree",
        [
            ("model", string("Microsoft Lumia 950")),
            ("compatible", string("microsoft,lumia950", "qcom,msm8992")),
            ("#address-cells", u32(2)),
            ("#size-cells", u32(2)),
        ],
        children,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "build/lumia950.adt")
    parser.add_argument("--dram-size", type=lambda value: int(value, 0), required=True,
                        help="DRAM the firmware reports to the OS, not the 3 GiB marketing figure")
    parser.add_argument("--dram-base", type=lambda value: int(value, 0), default=DRAM_BASE)
    parser.add_argument("--cpu-regs", default=DEFAULT_CPU_REGS,
                        help="comma-separated MPIDR affinity values, boot CPU first")
    parser.add_argument("--unique-chip-id", type=lambda value: int(value, 16),
                        help="64-bit platform seed in hex; XNU blocks gethostuuid() without it")
    parser.add_argument("--chip-id", type=lambda value: int(value, 0),
                        help="32-bit platform identity seed")
    parser.add_argument("--owned-memory-size", type=lambda value: int(value, 0),
                        help="contiguous window request, 256 MiB to 8 GiB in 2 MiB steps")
    parser.add_argument("--root-match-volume-uuid", type=uuid.UUID)
    parser.add_argument("--psci-method", choices=["hvc", "smc", "none"], default="hvc")
    parser.add_argument("--osenvironment", choices=OSENVIRONMENTS, default="main")
    parser.add_argument("--crypto-hash-method", choices=CRYPTO_HASH_METHODS, default="sha2-384")
    parser.add_argument("--data-ramdisk", action="store_true")
    parser.add_argument("--packed-data-ramdisk", action="store_true")
    parser.add_argument("--trust-cache", action="store_true")
    parser.add_argument("--owner-trust-cache", action="store_true",
                        help="accepted from the payload contract; it still only reserves TrustCache")
    parser.add_argument("--gpu-addon", action="store_true",
                        help="accepted from the payload contract; no ADT property is defined for it")
    parser.add_argument("--ipad-identity", action="store_true",
                        help="accepted from the payload contract; this board is not an iPad")
    parser.add_argument("--persistent-data", action="store_true")
    parser.add_argument("--text-fixup", action="append", default=[])
    args = parser.parse_args()

    if args.data_ramdisk and args.packed_data_ramdisk:
        parser.error("--data-ramdisk and --packed-data-ramdisk are alternatives")
    if args.persistent_data and not (args.data_ramdisk or args.packed_data_ramdisk):
        parser.error("--persistent-data needs --data-ramdisk or --packed-data-ramdisk")
    if args.owned_memory_size is not None and not (
            0x10000000 <= args.owned_memory_size <= 0x200000000 and args.owned_memory_size % 0x200000 == 0):
        parser.error("--owned-memory-size is 256 MiB to 8 GiB in 2 MiB steps")
    if args.chip_id is not None and not 0 <= args.chip_id < 1 << 32:
        parser.error("--chip-id is a 32-bit value")
    if args.unique_chip_id is not None and not 0 <= args.unique_chip_id < 1 << 64:
        parser.error("--unique-chip-id is a 64-bit value")

    if args.gpu_addon:
        print("note: --gpu-addon has no ADT property; nothing is emitted for it", file=sys.stderr)
    if args.ipad_identity:
        print("note: --ipad-identity does not apply to MSM8992; the product node stays the Lumia", file=sys.stderr)

    tree = build_tree(
        dram_size=args.dram_size,
        dram_base=args.dram_base,
        cpu_regs=parse_cpu_regs(args.cpu_regs),
        unique_chip_id=args.unique_chip_id,
        chip_id=args.chip_id,
        owned_memory_size=args.owned_memory_size,
        root_match_volume_uuid=args.root_match_volume_uuid,
        psci_method=args.psci_method,
        osenvironment=args.osenvironment,
        crypto_hash_method=args.crypto_hash_method,
        data_ramdisk=args.data_ramdisk,
        packed_data_ramdisk=args.packed_data_ramdisk,
        trust_cache=args.trust_cache or args.owner_trust_cache,
        persistent_data=args.persistent_data,
        text_fixups=tuple(args.text_fixup),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)

    cpus = len(parse_cpu_regs(args.cpu_regs))
    print(f"lumia950 ADT: {len(tree)} bytes, {cpus} CPUs, dram {args.dram_size:#x} at {args.dram_base:#x}")
    if args.unique_chip_id is None or args.chip_id is None:
        print("warning: no chip identity; XNU seeds IOPlatformUUID from it and gethostuuid() blocks without it",
              file=sys.stderr)
    print("note: ArmCpu GICv2 integration still needs an XNU build and handset IRQ validation", file=sys.stderr)
    if cpus > 1 and args.psci_method != "none":
        print("note: MSM8992 PSCI is a subset over HVC with no SYSTEM_OFF/SYSTEM_RESET (README blocker 4)",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
