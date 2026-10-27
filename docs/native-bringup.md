# Firmware-native GPU and XNU bring-up

The active target is the Lumia's own ARM64 firmware, then native XNU/iOS. The
Windows demonstration is not a substitute and is not part of this boot tree.

## Three-entry firmware menu

`scripts/stage-native-bringup.py` prepares a reversible FAT file tree with:

1. **Native Adreno hardware test**: `LumiaGpuPower.efi` runs on the firmware's EL1
   environment, then exits boot services and operates the hardware natively.
2. **XNU kernel - no iOS userspace yet**: DarwinBoot hands off to the source-built
   iOS 16.x XNU kernel. This is an early kernel/console/IOKit boot attempt, not a
   SpringBoard, APFS-root, application or accelerated compositor milestone.
3. **Exit to firmware**: the default. Neither test automatically starts; the
   five-minute idle timeout returns to firmware.

The stage command never installs anything or alters a disk. Installation requires
separately verifying the phone's partition identity, backing up the existing
fallback and overwritten files, and copying only the staged FAT files. Do not
modify `UEFI.elf`, BootShim, stock Windows boot files, partition tables or BCD.
The old fallback must remain recoverable on the SSD before any replacement.

## Native hardware activation

This is the first EFI caller of the GPU power and A418 command-hardware code. It
is not another name for the link-only batch image or the Windows application.
Its steps are:

- Verify the measured MSM8992 MADT, boot A53, EL1h/MMU/timer, GOP, GCD MMIO
  reservations, actual Device identity translations and absence of conflicting
  UEFI RAM allocations. GPU registers are not read while the domains are off.
- Save the preflight. **Volume Up** deliberately authorizes the post-EBS test;
  another key cancels. No native SPMI transaction or GPU power/clock write occurs
  before this confirmation and successful firmware exit.
- Mask interrupts, install local EL1 exception vectors, and claim/open the same
  native RPM channel that passed the earlier handset handshake.
- Use a bounded native SPMI V1/channel-0 implementation, not retained firmware
  pointers. Require the expected FTSMPS2p5 peripheral at PMI8994 USID 3, `0x1700`.
  The direct graphics rail can be raised/enabled at **980000 microvolts**, the
  static Lumia board constraint. Existing adequate higher voltage is retained;
  unknown regulator/range/pin policy is refused. Only `0x1741`, `0x1745`, `0x1746`
  accept narrowly allowed native writes. No other PMIC rail is writable here.
- Request **clk2, ID 1, KHz=19200** through a correlated native RPM transaction.
  This dedicated graphics resource is used only after exclusive cold-GPU ownership;
  it is pinned, not restored to a guessed prior baseline. The APPS graphics vote
  may change. Shared configuration/memory-bus votes are deliberately **untouched**.
- Enable the OxiLi domains, retention and three branches using bounded polls.
  Read actual GPU identity, hold/program the A418 command hardware, and inspect
  inherited SMMU routing without modifying global routes, security or context banks.
- Display the actual completion/failure stage and register values. Recoverable
  synchronous MMIO data aborts return failure with ESR/FAR/ELR recorded. Unknown
  exceptions stop visibly. A non-returning physical bus transaction is not made
  recoverable by this exception handler.

Supply, clock vote and uncertain hardware state remain **pinned until reset**.
There is no guessed release or automatic failed-power rollback. Existing buses
are sufficient only as an inherited configuration-access path for this test;
their bandwidth, vote ownership and DMA suitability are **not qualified** by it.
The integrated `GpuBatch` still requires its separate DMA/bus/baseline contracts.

The result screen is CPU-drawn diagnostics. No microcode queue, DMA destination,
shader, GPU-rendered image, measured FPS or iOS support is claimed by this test.
SMMU routing may remain unsupported even after command hardware initialization.
Actual handset results must be recorded separately from host tests and EFI builds.

The rail mapping comes from the [Lumia board description](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/msm8994-msft-lumia-octagon.dtsi),
[PMI8994 SPMI description](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/pmi8994.dtsi)
and [regulator definitions](https://github.com/torvalds/linux/blob/v6.12/drivers/regulator/qcom_spmi-regulator.c).
Controller layout and execution environment come from the installed firmware's
[matching SPMI implementation](https://github.com/WOA-Project/Lumia950XLPkg/blob/0e7e24a/Driver/SpmiDxe/spmi.c)
and memory map. Static source facts are not represented as live handset observations.

## Source-only XNU entry

The previously merged 56 MB kernel is explicitly runtime/ABI/relocation-unqualified.
This stage **does not clear or bypass that guard**. It takes the separately linked
source-only kernel from the successful build, refuses any nonempty prelink segment
or unresolved native symbol, and uses its real ARM64 thread entry.

Apple `strip -S` removes debug symbols to fit the current EFIESP free space. The
stager independently verifies every non-linkedit runtime segment's bytes (excluding
the load-command header), virtual geometry, protections, entry, and all external
symbol values after stripping. It records input/output hashes. LLVM's current
strip implementation rejects this kernel; no lossy hand-written Mach-O rewrite
is substituted.

The ADT describes the measured six CPU affinities and GICv2, while `cpus=1` limits
the first boot. DarwinBoot must allocate an actual contiguous 512 MiB conventional
memory window, not assume ownership of every byte of the advertised 3 GiB. The
port identifier passed as `--unique-chip-id` is a loader/port value, not an Apple
ECID or a fuse modification. GOP geometry is read live by the loader.

No iOS root filesystem, APFS kext closure, trust cache or userspace is included in
this early boot tree. A kernel panic or root wait is possible and is evidence to
debug, not successful iOS. First obtain real kernel handoff/console/initialization
results, then resolve the complete runtime-qualified image and userspace path.
