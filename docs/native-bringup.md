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

`scripts/install-native-bringup.py` implements that file-only installation for
the measured boot state. It defaults to a dry run and requires the independently
read volume and partition UUIDs. `--apply` first backs up and rehashes every
existing EFIESP file on the separate SSD, then verifies the new payloads before
replacing the fallback last. Final readback checks both the staged files and all
other original files. Unexpected disk identities, changed boot hashes, an
existing native tree, invalid stage hashes and insufficient space are refused.
It never formats, repartitions, modifies BCD or automatically reboots/unmounts.

## Native hardware activation

This is the first EFI caller of the GPU power and A418 command-hardware code. It
is not another name for the link-only batch image or the Windows application.
Its steps are:

- Verify the measured MSM8992 MADT, boot A53, EL1h/MMU/timer, GOP, GCD MMIO
  reservations, actual Device identity translations and absence of conflicting
  UEFI RAM allocations. GPU registers are not read while the domains are off.
  The matching firmware source maps MMSS only through `0xfd9dc000`, omitting
  `0xfdb00000..0xfdb1ffff`. If this GPU/SMMU aperture is GCD NonExistent and has no
  UEFI RAM conflict, reserve exactly those 128 KiB as MMIO and request UC page
  attributes through DXE before checking every Device translation. Existing RAM,
  reserved ownership, ambiguous descriptors and failed mappings are not overridden.
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
  Re-read the actual graphics supply after the clock ACK; an inadequate or changed
  regulator policy stops before domain/branch enable instead of trusting stale data.
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
All numbers on this diagnostic screen are hexadecimal. `PMIC TYPE/SUB` packs
type then subtype in two bytes; `RAIL IN/OUT CFG` packs range, voltage selector,
mode and enable in four bytes. Raw PMIC status, A418 last register/value and ELR
are retained alongside SMMU identifiers and the first four stream slots so a
single full-screen photo captures the failing configuration. Unreached fields
remain zero and are not represented as successful hardware reads.

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

The pinned loader's old green-bar wording incorrectly claimed kernel execution.
`ci/darwinboot-handoff-label.patch` corrects that message: the bar is painted by
the loader **before** branching. It is never accepted as proof of XNU boot.

### First real early-boot fault

The source-only image was executed through DarwinBoot in an A53/GICv2 QEMU VM.
A hardware breakpoint caught its first UBSan trap at `wfe_timeout_configure`,
before the recursive early exception-handler faults. The Lumia ADT's legitimate
identity bus has a zero parent base. Apple's `pe_identify_machine` treated that
as an absent SoC and returned before reading the CPU's `timebase-frequency`.
Consequently `flsll(0)` entered an invalid signed shift/subtraction calculation.
This was kernel instruction execution in emulation, not an on-handset boot.

The 8796 overlay now makes native architectural-timer clock discovery independent
of an Apple MMIO base. Unknown CPU/bus frequencies remain unknown, with their
derived divisions guarded; the measured 19.2 MHz timebase still comes from the
ADT. The legacy platform path retains its original gate. The test compiles the
hash-qualified Apple clock and WFE functions with UBSan enabled and a mock DT,
reproducing the old zero-base failure and checking both patched/legacy paths.
No trap, sanitizer, kernel instruction or qualification gate is disabled.

`scripts/test-native-xnu-entry.py` repeats the real A53/GICv2 instruction test on
a private read-only ESP copy, using named kernel breakpoints rather than pixels.
It stops at the first trap/panic/exception or `arm_vm_init` entry and records
registers, clock bytes, WFE event index and kernel hash. The original kernel
reproduces the same UBSan stop. Reaching VM initialization's entry proves only
the preceding early initialization, not completion of VM, IOKit, an actual
handset boot or iOS userspace.

The source-only configuration also selects the actual source crypto initializer.
It cannot wait for a prelinked provider that is absent from this image. This does
not qualify the separate merged candidate's bootstrap or imported component ABI;
that candidate remains explicitly unqualified and is not staged here.
