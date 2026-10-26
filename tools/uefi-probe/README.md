# Lumia firmware probe

For GPU bring-up, `PROBE_ENTRY=LumiaGpuProbe` builds a separate read-only hardware
preflight. It checks the MSM8992 ACPI layout before board-specific MMIO, reads rails and
clock branches before touching the GPU, and conditionally captures idle nonsecure SMMU
state. Version 2 also queries the firmware's Qualcomm clock-name registry without calling
enable, rate, reset, or parent-changing methods. Registry presence does not mean a clock
is running. It saves a new `EFI/LumiaProbe/gpu-report-00.txt` through `gpu-report-63.txt`
on its own boot filesystem, skipping existing reports. No GPU, power, clock, SMMU or
firmware-variable writes occur. It does not run the submission backend or provide
acceleration. See [GPU bring-up](../../docs/gpu-bringup.md).

This is a standalone AArch64 EFI application, not XNU and not an iOS image. It measures
the firmware environment before staging a kernel: current exception level, boot CPU,
timer frequency, GOP framebuffer geometry, firmware memory-map summary, protocol counts,
and ACPI/SMBIOS entry addresses. In particular, it resolves DarwinBoot's EL1 requirement
without attempting a kernel handover.

The default `LumiaProbe.efi` makes no disk writes, changes no firmware variables, does not call
`ExitBootServices`, and returns to its caller after a key press. Its memory summary keeps
conventional, loader/boot-services, and runtime memory separate; runtime or reserved
descriptors must not be treated as free RAM. Firmware initialization during ordinary boot
is outside this application's control.

## Build

Requirements: Clang with an AArch64 Windows target, `lld-link`, Python 3, and the official
EDK II `MdePkg/Include` headers. No Xcode SDK or EDK II BaseTools build is needed for this
standalone application. This does not change the XNU driver's CI requirements.

```sh
UNIXNU_EDK2=/path/to/edk2 bash scripts/build-probe.sh /path/to/probe-output
bash scripts/test-probe.sh
```

The named executable is `esp/EFI/LumiaProbe/LumiaProbe.efi`. The same image is staged as
`esp/EFI/BOOT/BOOTAA64.EFI` for removable-media boot and emulator testing. The build script
does not install either file on the phone. `check-probe-image.py` checks the PE architecture,
subsystem, executable entry point, and section bounds; it does not prove hardware support.

The QEMU smoke test boots that exact image, checks its EL1/GOP/memory report, sends a key,
and requires the application to reach its return path. It accepts both GICv2 and GICv3:

```sh
python3 scripts/test-probe-qemu.py --esp /path/to/probe-output/esp \
    --firmware /path/to/edk2-aarch64-code.fd --gic-version 2 --log probe-qemu.log
```

These are EFI application tests, not the XNU driver compile or a phone boot test.

## GPU report regression tests

The v1 handset test created an empty report and returned `EFI_INVALID_PARAMETER`. It did
find its own filesystem; v1 did not record which later file operation failed. Version 2
creates only unused names and does not truncate existing files, call `GetInfo`/`SetInfo`,
or seek. It logs the exact save stage and EFI status to the screen. A short write, flush
failure, or close failure is not reported as success. The report contains measurements
through `GPU PROBE COMPLETE`; the later save result is shown on screen, not in that file.

```sh
UNIXNU_EDK2=/path/to/edk2 bash scripts/test-report.sh
UNIXNU_EDK2=/path/to/edk2 PROBE_ENTRY=LumiaGpuProbe \
    bash scripts/build-probe.sh out/gpu-preflight-v3
python3 scripts/test-probe-qemu.py --application gpu --esp out/gpu-preflight-v3/esp \
    --firmware /path/to/edk2-aarch64-code.fd --writable-report-test
```

The writable test converts a **private copy** of the ESP tree into a sparse raw FAT16
image with `qemu-img`, boots it twice, and independently reads back the files without
mounting a host volume. Saved bytes must match the actual EFI console transcript, and
all previous reports and existing files must remain unchanged. Writable directory-backed
`fat:rw:` is deliberately not used: it produced incorrect file data in local QEMU 10.2
testing while the equivalent raw block image passed. Normal read-only smoke tests still
require `EFI_WRITE_PROTECTED` at `CREATE_FILE`. Neither test targets a physical disk.

GPU preflight v3 also queries the Qualcomm SMEM protocol when the platform check passes.
It reads the 64-entry channel-allocation table and reports one unambiguous APPS-RPM
channel's ID, reference count, control buffer and FIFO. It refuses malformed or
overlapping layouts and never opens the channel, writes shared memory or rings the IPC
doorbell. These addresses are observations from boot services, not a qualified native
post-handoff mapping. The v3 handset report confirmed channel 4, an 88-byte control
block at `0xfc428000`, and a 2048-byte FIFO allocation at `0xfc428200`. The GPU remained
off; no channel was opened or resource vote sent. See
`docs/hardware/2026-10-26-smem-preflight-v3.json` for the evidence and limits.

The clock lookup ABI is pinned to Lumia950XLPkg revision
`cfc68a4d19fc7e8645bff6d8fce7308ad8494a76`, `Include/Protocol/QcomClock.h` and
`Driver/ClockDxe/clock.c`. Only its first `clk_get(const char *)` entry is called, with
no protocol `This` argument. The published platform clock table does not establish a
working GPU power-on path; the installed handset's registry still needs measurement.

## Post-firmware display test

`LumiaHandoff.efi` is a separate, deliberate test. It requires a linear RGB/BGR GOP
framebuffer and EL1. At its prompt, Volume Up starts the test; any other key returns
without exiting firmware. It first draws an orange `BEFORE EBS` screen, captures a fresh
memory-map key, and calls `ExitBootServices`. After success it masks interrupts and draws
a green `POST EBS OK` screen plus a moving bar directly into the framebuffer, with no
further firmware calls. It keeps the existing MMU mappings and does not claim to test
XNU's page tables, interrupt controller, or a native display driver.

No disk files or firmware variables are written by the application. **After starting the
handoff test, force-reboot the phone with Volume Down + Power to leave it.** A frozen
orange screen, black screen, red error screen, and moving green screen are distinct results;
record the actual result rather than treating a leftover picture as a successful handoff.

```sh
UNIXNU_EDK2=/path/to/edk2 PROBE_ENTRY=LumiaHandoff \
    bash scripts/build-probe.sh out/handoff
python3 scripts/test-handoff-qemu.py --esp out/handoff/esp \
    --firmware /path/to/edk2-aarch64-code.fd --output out/handoff/qemu-gic2
python3 scripts/test-handoff-qemu.py --esp out/handoff/esp \
    --firmware /path/to/edk2-aarch64-code.fd --output out/handoff/qemu-cancel --cancel
```

The emulator uses a linear RAM framebuffer. It checks two QMP framebuffer captures for
the green post-EBS marker and changing heartbeat, rather than expecting UEFI console output
after firmware exit. The normal diagnostic remains the default fallback unless
`PROBE_ENTRY=LumiaHandoff` is specified.

## Native RPM handoff test

`LumiaRpmHandoff.efi` is separate from the read-only GPU probe. It tests the native SMD
channel needed to request GPU resources after firmware exit. **It does not power the
GPU, render a GPU frame, boot XNU or provide iOS.** See [native power work](../../docs/native-gpu-power.md).

Before enabling anything, it requires the measured channel-4 profile, Device
stage-1 read/write translations, UC-capable DXE GCD MMIO classification, no UEFI RAM
allocation conflicts, and the expected Lumia
950 platform. It saves its preflight, including raw PAR values and firmware-side channel
states/indices, to a new `gpu-report-NN.txt` using the existing append-only report writer.
There are no channel, FIFO or IPC writes while firmware services are live.

Version 2 holds early failures on screen with a failed-stage/status marker rather than
immediately falling back to the UEFI menu. It also corrects v1's invalid `PAR_EL1.NS`
requirement: that bit is UNKNOWN for Non-secure translations. The caller must separately
qualify its Non-secure EL1 boot context; the bit is not used as proof of either world.
The recovered v1 report confirms a different actual stop: valid translations passed,
but the strict UEFI MMIO-descriptor requirement failed. V2 queries the separate DXE GCD
resource map and logs its classification/capabilities instead of assuming every MMIO
range appears with UC attributes in `GetMemoryMap`. It still refuses any overlapping
UEFI RAM, loader, boot-services or runtime allocation. No GCD map is modified.

V2 ran on the handset and reached native post-exit control reads. The result was local
`CLOSED (0)`, peer `CLOSING (4)`, TX read/write `0x60` and RX read/write `0x28`: both FIFOs
were empty, but v2's stricter claim refused the peer state. V3 accepts this idle boundary
only with exclusive ownership, valid mappings, both DTR signals zero and eight identical
empty snapshots. It also handles peer `OPENING -> OPENED` as two phases under one deadline,
and reports post-exit DTR, snapshot validity and memory-binding state. No peer state or
unread FIFO is overwritten. V3's native handshake is not yet qualified on the handset.

At `RPM HANDOFF READY`, **Volume Up starts**; any other key cancels. The test disables
the watchdog, captures a fresh memory map and exits boot services. It then masks interrupts,
rechecks the mappings and idle/empty state, and performs one bounded native open/close
handshake through shared-memory control words and the RPM IPC doorbell. It never sends a
resource packet, adopts a still-open channel, or resets unread data. Mappings remain pinned
on uncertain failure. After local close with an idle peer, eight stable snapshots qualify
retirement of local access rather than reuse. Peer CLOSING is not falsely recorded as
peer CLOSED. The RPM shared storage stays reserved; no resource vote or rail is released.

The orange `BEFORE EBS`, blue `POST EBS - CHECKING RPM`, and final result are distinct
stages. A green `NATIVE RPM CHANNEL OK` requires claim, open, close and retirement all to
succeed; other results stop without proceeding to GPU registers. **The final screen is
CPU-drawn diagnostic output, not GPU acceleration.** Its moving bar is only a liveness marker.

The preflight text file does **not** contain post-exit results: file services are gone at
that point. Photograph the final screen, then force-reboot with Volume Down + Power.
No firmware functions are called after successful exit. The firmware's inherited mappings
are kept; this does not qualify XNU's future mappings, native IRQs or resource-vote policy.

```sh
UNIXNU_EDK2=/path/to/edk2 PROBE_ENTRY=LumiaRpmHandoff \
    bash scripts/build-probe.sh out/rpm-handoff-v3
bash scripts/test-gpu.sh
UNIXNU_EDK2=/path/to/edk2 bash scripts/test-report.sh
python3 scripts/test-probe-qemu.py --application rpm --esp out/rpm-handoff-v3/esp \
    --firmware /path/to/edk2-aarch64-code.fd --gic-version 2
```

QEMU exercises only the unsupported-platform return; the Lumia RPM is not emulated.
The 685-check memory/handoff suite uses a simulated peer; no handset handshake success is
claimed from those tests. The standalone app must be installed deliberately; selecting
`PROBE_ENTRY` stages only an output directory and does not write a physical disk.

## On the phone

Use the already-installed Lumia950XLPkg ARM64 UEFI. The Windows Phone Developer Menu is
not an ARM64 EFI launcher; on the inspected handset, the first boot menu's `Windows 10`
entry runs `BootShim.efi`, which starts `UEFI.elf`.

Prefer launching `LumiaProbe.efi` by its full path from an existing ARM64 EFI file browser
or shell. If rEFInd is absent and the firmware falls straight through to Windows, its
removable-media fallback can launch `\EFI\BOOT\BOOTAA64.EFI` from a mounted FAT volume.
Inspect and back up any existing file at that name before using the fallback. Do not
replace `UEFI.elf`, `BootShim.efi`, `bootarm.efi`, Windows files, or the BCD to run this probe.

Photograph the complete probe report. The `CurrentEL`, GOP, framebuffer address, and memory
lines are required inputs to the next boot stage. A firmware report is not proof that the
framebuffer survives `ExitBootServices` or that any XNU driver works.

If a fallback copy was used, remove only that new file after testing so the existing
Windows boot path can resume. Preserve a pre-existing fallback loader rather than deleting it.
