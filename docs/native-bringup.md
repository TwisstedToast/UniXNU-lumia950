# Firmware-native GPU and XNU bring-up

The active target is the Lumia's own ARM64 firmware, then native XNU/iOS. The
Windows demonstration is not a substitute and is not part of this boot tree.

## Three-entry firmware menu

`scripts/stage-native-bringup.py` prepares a reversible FAT file tree with:

1. **Native Adreno hardware test**: `LumiaGpuPower.efi` runs on the firmware's EL1
   environment, then exits boot services and operates the hardware natively.
2. **Experimental XNU kernel - no iOS userspace**: DarwinBoot hands off to the source-built
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
The backup is `EFIESP.tar`, with every contained file rehashed, plus an independent
`fallback-rescue.efi`. A loose-file backup on this Mac changed three AppleDouble
metadata sidecars after creation; verification caught this before any phone
writes. Keeping the exact original sidecar bytes inside the archive prevents
macOS from treating them as live metadata while retaining them for recovery.

`scripts/update-native-bringup.py` updates an **exactly matched previous stage**,
not an arbitrary existing EFI directory. Both stages must pass the same hashes,
source-kernel integrity checks and explicit-choice menu contract. Only the
experimental GPU image, kernel, ADT and manifest may change; loader, fallback,
BCD, BootShim and firmware must remain byte-identical. The existing USB root flag
is checked first. The update archives and verifies the **known nonmetadata files**
from the original verified backup inventory plus the existing native stage on the
separate SSD. It requires that original `before.json` as `--original-inventory`,
checks its device identity and rejects escaping paths. It never walks/stats the
FAT tree or opens AppleDouble/cache files: macOS enumeration caused unrelated
files to be cached as directories even when their bytes verified on a cold mount.
It checks the native tree's fixed directory names without statting their metadata
entries. Unknown non-native additions are not represented as archived/verified;
the report explicitly names its known-file scope. The charging log can legitimately
change between boots; the update backs up its current bytes, not a stale log.
With less than one spare kernel's worth of space, only the backed-up experimental
kernel is removed before replacement. This part is not atomic: interruption can
leave that experimental entry unavailable, while the unchanged menu's default
still exits to firmware. `--previous-stage` and a verified SSD backup are required;
dry-run remains the default. The original full archive/rescue is retained separately.

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
- Request **clk2, ID 1, KHz=19200** through a correlated native RPM transaction
  **before** establishing the direct graphics voltage.
  This dedicated graphics resource is used only after exclusive cold-GPU ownership;
  it is pinned, not restored to a guessed prior baseline. The APPS graphics vote
  may change. Shared configuration/memory-bus votes are deliberately **untouched**.
  The first handset run reached this ACK but then read only **700000 microvolts**,
  despite the earlier 980000-microvolt verification in that run's control flow.
  It stopped before domain, branch, GPU or SMMU activation. This demonstrates a
  stale supply dependency, not which remote component changed the selector.
  The revised fixed-rate handoff therefore completes the one clock request first,
  then reads/establishes the direct supply once. It requires **64 unchanged ready
  snapshots, spaced 250 microseconds apart**, under one bounded SPMI poll budget.
  A competing voltage/mode/enable change invalidates readiness; it is never fought
  with repeated supply writes. No further RPM request is issued after establishment.
  Unknown pin policies and missing ACKs stop before any direct supply writes.
- Enable the OxiLi domains, retention and three branches using bounded polls.
  Revalidate the same actual supply before opening GPU register access and again
  after command-hardware initialization. An invalidated rail remains pinned and
  cannot be silently requalified/retried in this run.
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
The revised ordering is an experimental fixed-clock configuration-access path,
not proof of permanent remote voltage ownership, dynamic frequency scaling or a
DMA/rendering qualification. The recorded handset stop and host-only follow-up
are in `docs/hardware/2026-10-27-native-gpu-supply.json`.
All numbers on this diagnostic screen are hexadecimal. `PMIC TYPE/SUB` packs
type then subtype in two bytes; `RAIL IN/OUT CFG` packs range, voltage selector,
mode and enable in four bytes. Raw PMIC status, A418 last register/value and ELR
are retained alongside SMMU identifiers, cumulative stable-supply samples and the first four stream slots so a
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

The first-boot ADT exposes only the measured affinity-zero A53 and GICv2, with
`cpus=1`. The six physical affinities remain documented by the firmware probes,
but this entry does not invent a working secondary-CPU PSCI/HVC conduit.
DarwinBoot must allocate an actual contiguous 512 MiB conventional
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

The next actual VM assertion exposed a missing `/chosen/memory-map` node. The
generator now always emits that node, empty when there is no real RAMDisk or
TrustCache; it never invents physical placeholder contents. The sealed build
from CI run `40493229820`, source `8742e38`, then entered and **returned from**
`arm_vm_init` under A53/GICv2 emulation. Its 19.2 MHz DT timebase produced WFE
event index 4. This proves neither an accurate QEMU hardware clock rate nor a
handset result; it verifies the source-kernel initialization path.

Further real IOKit execution exposed missing interrupt-parent references. The
GIC now has a local software `AAPL,phandle` and zero address cells; all three
peripheral nodes explicitly reference it. These are registry references, not
guessed physical interrupt routes or proof of device IRQ delivery. The native
drivers remain polling/identity bring-up. `--crypto-post` observes the real
provider entry/return and status, stopping on intervening traps or a bounded
timeout; a sealed field or a timeout alone is never accepted as POST success.
The kernel menu entry is explicitly experimental, not a ready iOS installation.
The next exception was traced to `sha1_init`, ELR `0xfffffff00785dbb8`, FAR `0x28`:
the real crypto dispatch table was still unregistered. In this 16.x startup order,
`PE_init_iokit` runs before `EARLY_BOOT`, while IOKit already needs SHA1. The line's
source-provider overlay therefore schedules the unchanged full POST/bootstrap at
`SYSCTL` rank last, after its allocator/locks/thread-call prerequisites but before
IOKit. This changes initialization order, not crypto implementations or checks;
the runtime result is established separately from the build and static sealing.

That corrected kernel is now built successfully in CI run `40498673285`, source
`9cfe75c`, SHA-256 `03e48c7ce3ae3365e168da14be30488401d96d9e658858be7186f55ccc68db5d`.
An actual A53/GICv2 run observes VM entry/return, then the real source crypto
bootstrap entry/return with **status 0**, then `PE_init_iokit` entry/return. The
observation tool's `--finish-iokit` requires all six milestones in that order;
no guest instruction, kernel return value, POST or exception is patched to pass.
The QEMU CPU is now explicitly configured with `cntfrq=19200000`, and the test
independently reads `CNTFRQ_EL0` to reject a frequency mismatch with the ADT.
These are emulator initialization milestones, not completed driver matching,
a mounted root filesystem, handset XNU execution or iOS userspace.

The source-only configuration also selects the actual source crypto initializer.
It cannot wait for a prelinked provider that is absent from this image. This does
not qualify the separate merged candidate's bootstrap or imported component ABI;
that candidate remains explicitly unqualified and is not staged here.
The 16.x build seals the real `mach.development.bcm2837.unstripped` output; this
line has no 17.x `.noctf` artifact. The integrity-sealing step is retained, not
skipped to work around the different output filename.
The overlay also carries `CFLAGS_EXTRA`/`SFLAGS_EXTRA` into the 8796 make rules:
unlike the 17.x rules the base script targets, the original 16.x rules silently
ignored these variables. A linked archive alone therefore did not establish that
its source bootstrap was compiled. The make-expansion regression and mandatory
bootstrap-symbol/POST-field checks now catch that difference before accepting an
image. Runtime source-provider POST remains a separate required observation.
The native stager and installer both independently require the actual provider
symbols and matching POST HMAC in the image. A JSON flag, a linked archive or
the known older source-only image cannot satisfy this gate. This is a static
integrity/initialization contract, never a claim that runtime POST already passed.
