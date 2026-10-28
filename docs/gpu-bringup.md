# Lumia 950: native GPU, then touchscreen

The current order is **real native GPU output, then real touchscreen input, then iOS**.
The GPU milestone requires completed GPU work producing visible frames on the handset,
not a Windows application, a CPU-drawn framebuffer animation or a simulated test.
A command-processor memory-write proof is useful but is not a shader-rendered frame.
Completed-frame throughput and display/presentation rate must be measured separately;
the camera's recording rate and a timed CPU loop do not establish 60 FPS rendering.

After GPU output works, touch must come from the physical controller's contact reports
and native interrupts, including down/move/up coordinates while the display remains live.
Firmware pointer events and desktop mouse input do not establish native touchscreen support.
The existing experimental XNU artifacts remain available, but kernel/userspace integration
is deferred until those hardware milestones. No iOS or Metal handset success is claimed.

## Implemented in this pass

- Correct A4xx PM4 type-3 headers: `(payload_words - 1) << 16`, opcode in bits 15:8.
  The old implementation and its tests agreed on the wrong encoding. Independent golden
  words now include `CP_WAIT_FOR_IDLE = 0xc0002600` and a three-word `CP_EVENT_WRITE`
  header of `0xc0024600`.
- Correct ARMv7 short-descriptor B/C memory attributes. Those bits belong in the final
  descriptor, not in the attribute subfield before its level-dependent shift.
- Refused microcode replacements clear all previously prepared load-plan fields. The
  A418 `CP_ME_INIT` packet sets payload word 14 to `0x20000000`, matching Qualcomm's
  shared A3xx/A4xx `a3xx_rb_init` protected-register handling. Both have host regressions;
  neither has been executed on the handset.
- Actual A418 firmware selection and verification. Qualcomm's A418 entry selects
  `a420_pm4.fw` and `a420_pfp.fw`; these are now fetched or imported with pinned sizes,
  hashes, source URLs, and the accompanying license. A zero container header is valid.
  PM4's version is word 1, PFP's is word 5, and both hardware downloads skip only word 0.
- `GpuIo`: shared, read-only power/clock preflight. Neither the native driver's identity
  path nor the EFI diagnostic touches GPU registers if either rail is down, collapse is
  pending, or any required branch is disabled/halted. Native MMIO now uses volatile,
  aligned accesses and ARM barriers instead of nonvolatile `memcpy`.
- `LpaeCore`: owned, three-level, 4 KiB ARM stage-1 page tables for a 32-bit GPU address
  space and physical addresses below 4 GiB. Mappings include permissions and cache
  attributes; bounds, table ownership, overlaps, and pool exhaustion are checked before
  mutation. Lookup and unmapping are provided. This is separate from XNU's CPU page size.
- `SmmuContext`: register-backed configuration of **already disabled, exclusively owned**
  user/privileged context banks; TTBR/ASID, TCR/TCR2, MAIR, bounded TLB synchronization,
  hardware ATOS/PAR translation checks, and explicit disable. Active contexts and the
  spare bank are refused. A failed disable retains ownership so its memory is not freed.
- `Submission`: register-backed firmware download, ring configuration, micro-engine
  initialization, ring wrap, DMA synchronization, a bounded `CP_MEM_WRITE` fill, polled
  fences, interrupt-status fault detection, timeout poisoning, data verification, and
  explicit halt. The destination is first poisoned with the inverse pattern; unchanged
  CPU-initialized memory cannot count as a successful GPU fill. This is a command-processor
  memory-write test, **not a shader draw, blitter, or Metal implementation**.
- A compiled ARM64 `LumiaGpuProbe.efi` that checks the MSM8992 ACPI CPU/GIC layout before
  any board-specific MMIO. It records rails and clocks, conditionally records GPU/CP and
  idle nonsecure SMMU context state. Version 2 queries the firmware clock registry and
  saves a fresh `EFI/LumiaProbe/gpu-report-NN.txt` on its own boot filesystem, with exact
  failure-stage reporting. It does not enable clocks, load GPU firmware, configure SMMU
  contexts, exit boot services, or change firmware variables.

- Native RPM request/ACK and word-access SMD transport cores now cover the identified
  graphics-parent resource dependency. They include bounded post-firmware channel
  reopening and reject stale/malformed replies and uncertain completion. Hardware
  SMEM bindings, shared vote policy and a Lumia-qualified rail profile remain pending;
  this is not an activated power driver. See [native GPU power](native-gpu-power.md).

The new backend sources are in the XNU archive builder and the host-test workflow. The
standalone EFI build also cross-compiles them for AArch64. That is not a compile of the
IOKit driver against XNU, nor a hardware execution of the backend.

## Handset result: 2026-10-26

The user's GPU preflight v1 photograph records:

| Register | Value | Interpretation |
| --- | --- | --- |
| GDSCR_GX / GDSCR_CX | `0x00282001` / `0x00282001` | Both POWER_ON bits clear; software collapse requested |
| CORE_CBCR | `0x80000ff0` | Halted, software enable clear |
| AHB_CBCR | `0x00008000` | Halt bit clear, but software enable also clear |
| TIMER_CBCR | `0x80000000` | Halted, software enable clear |

The preflight correctly stopped before reading any GPU or SMMU window. This is a concrete
power/clock prerequisite, not evidence of a dead GPU. The platform message verifies ACPI
topology only; actual interrupt delivery is a separate test. The photo and subsequent
empty report-file inspection are recorded in
`hardware/2026-10-26-gpu-preflight-v1.json`; these same register values are regression
fixtures in the host backend tests.

The report returned `EFI_INVALID_PARAMETER`. Its presence as a zero-byte file proves
filesystem discovery and creation succeeded, but v1 did not log the precise later failing
operation. Version 2 avoids metadata rewriting and adds stage/status reporting, with
mock-protocol tests and two-boot EFI file persistence tests on raw emulated FAT media.

Before enabling these resources, establish the installed firmware's clock registry,
the GPU parent clock/rate, CX/GX supply and RPM/interconnect ownership, and a bounded,
reversible transition sequence. The v2 lookup asks only for `oxili_gfx3d_clk`,
`oxilicx_ahb_clk`, and `oxili_rbbmtimer_clk`. A listed name is not a running clock;
an absent name does not mean the hardware is missing. Published Lumia UEFI clock/GDSC
sources are reference material, not a qualified GPU power sequence for this boot.

### V2: report recovery and clock registry confirmed

`PXL_20261026_165103658.jpg` shows `REPORT_STAGE=COMPLETE` and EFI success. The
independently recovered `gpu-report-00.txt` is 628 bytes, SHA256
`eb6c4dc7b84b56b0b20a92651f7ac277332f7d7d5b4a4120f646ec341a601680`, and matches
the photographed measurements. Report persistence is now demonstrated on the handset,
not just in the emulator. The old zero-byte v1 report remains untouched.

The Qualcomm clock protocol is present, but all three queried names return null.
This rules out using those exact registry entries for the next power-on attempt. It
does not establish that every possible alias or firmware power interface is absent.
Both domains remain `0x00282001`; core and timer values are unchanged. AHB is now
`0x80008000`, so **all three branches are halted**. V1's `0x00008000` AHB observation
must not be silently reused for this boot. Neither diagnostic programmed these clocks.

Evidence is in `hardware/2026-10-26-gpu-preflight-v2.json`. Host regression tests replay
both observed branch states and the present-protocol/three-missing-names combination.
The next implementation dependency remains a native, ownership-qualified power and
clock provider, including the external graphics parent and CX/GX supply/interconnect
requirements. Calling a null firmware clock handle or only clearing a collapse bit is
not that implementation. No new EFI application is needed merely to repeat this result.

## Validation and reproduction

```sh
bash scripts/test-gpu.sh
python3 scripts/adreno-firmware.py --fetch --output out/adreno-firmware
UNIXNU_EDK2=/path/to/edk2 PROBE_ENTRY=LumiaGpuProbe \
    bash scripts/build-probe.sh out/gpu-preflight
python3 scripts/test-probe-qemu.py --application gpu --esp out/gpu-preflight/esp \
    --firmware /path/to/edk2-aarch64-code.fd --gic-version 2
UNIXNU_EDK2=/path/to/edk2 bash scripts/test-report.sh
python3 scripts/test-probe-qemu.py --application gpu --esp out/gpu-preflight/esp \
    --firmware /path/to/edk2-aarch64-code.fd --writable-report-test
```

`test-gpu.sh` uses ASan/UBSan and includes an independently decoded simulated command
processor. It exercises mapping boundaries and atomic refusal, firmware words, register
ordering, ownership checks, halted clocks, faults, data corruption despite a completed
fence, counter/ring wrap, timeout, SMMU translation faults, and platform mismatches. The
Python tests verify firmware parsing and rejection. The QEMU test proves that this
application **declines** non-Lumia MMIO and returns; QEMU does not emulate an Adreno 418.

## What is still required before executing a GPU workload

The submission/context APIs are internal bring-up components, single-caller and
synchronous. They are not a public user-client ABI. Their caller must supply:

1. Exclusive ownership of a correctly identified A418; no firmware or other driver may
   still submit work or change its clocks. Power rails, RPM/interconnect votes, parent
   clocks, and branch clocks must already be established and remain held.
2. The A418 hardware initialization: VBIF, UCHE, SP/TP power and clock controls, protections,
   and a reset/idle CP with zero ring pointers. The CP backend does not pretend that
   downloading its firmware initializes the entire GPU.
3. Verified nonsecure SMMU stream routing/security ownership. Context-bank programming
   alone does not establish stream routing or satisfy any required TrustZone handoff.
   Do not disable a bank still owned by another component merely to pass the gate.
4. Prepared, pinned, owned DMA regions whose GPU translations match their physical backing.
   The ring, fence, destination, and page tables must be distinct. The mapping callback
   must verify actual mappings, not return a constant. Clean/invalidate callbacks must
   implement the platform's actual coherency requirements and use proper barriers.
5. Halted/idle GPU and synchronized disabled contexts before releasing any associated
   buffer. After timeout or uncertain I/O completion, retain the allocations until the
   hardware is known quiescent; a failed software call does not cancel a DMA request.

The default native driver still publishes **no GPU service** and does not invoke this
experimental submission backend. Enabling it before the hardware preconditions are
measured would not accelerate iOS; it would risk an opaque interconnect or DMA hang.

## From bring-up to wallpaper and effects

The remaining system path is: qualified XNU entry and GICv2 interrupts, working storage
and the matching iOS root/userland, a framebuffer/IOSurface presentation path, then
backboardd/SpringBoard with a compatible accelerated graphics stack. The existing public
16.7.16 kernel artifact explicitly records `loadable: false`; its source build is useful
evidence, not proof of a bootable phone OS. It does not include these new source changes.

For acceleration, Mesa's Freedreno A4xx/IR3 code is a reference for rendering and shader
compilation. **Turnip does not support A4xx.** The old Metal-to-Vulkan-to-Turnip idea is
not an implemented route for this phone. A compatible iOS 16 Metal plugin/shader path,
IOSurface memory sharing, submission isolation, scheduling/fault recovery, and presentation
still need development. No placeholder plugin or fake Metal device is registered here.

The user's `PXL_20261026_134638263.jpg` proves that the handoff application's CPU writes
reach the framebuffer after `ExitBootServices` with firmware MMU mappings retained. It
does not prove XNU mappings, a running iOS compositor, a moving heartbeat, or GPU work.

## Primary references

- [Qualcomm A418 model/firmware selection](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/gpu/msm/adreno-gpulist.h)
- [MSM8992 clock registers](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/clk/qcom/clock-mmss-8992.c)
- [MSM8992 context banks](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/arch/arm/boot/dts/qcom/msm-iommu-v1.dtsi)
- [Qualcomm A4xx initialization](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/gpu/msm/adreno_a4xx.c)
- [A4xx micro-engine initialization](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/gpu/msm/adreno_a3xx.c)
- [Firmware loading/container layout](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/gpu/msm/adreno_ringbuffer.c)
- [Linux PM4 headers](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/adreno/adreno_gpu.h)
- [Linux A4xx fence sequence](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/adreno/a4xx_gpu.c)
- [ARM SMMU registers](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu.h)
- [Linux LPAE mappings](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/io-pgtable-arm.c)
- [Linux short descriptors](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/io-pgtable-arm-v7s.c)
- [Pinned firmware](https://gitlab.com/kernel-firmware/linux-firmware/-/tree/20251021/qcom)
- [Lumia firmware clock protocol](https://github.com/WOA-Project/Lumia950XLPkg/blob/cfc68a4d19fc7e8645bff6d8fce7308ad8494a76/Include/Protocol/QcomClock.h)
- [Lumia firmware registry lookup](https://github.com/WOA-Project/Lumia950XLPkg/blob/cfc68a4d19fc7e8645bff6d8fce7308ad8494a76/Driver/ClockDxe/clock.c)
- [Freedreno and Turnip capabilities](https://docs.mesa3d.org/drivers/freedreno.html)
