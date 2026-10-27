# Integrated native GPU command batch

## Status

The allocation-free batch joins native SMEM/SMD ownership, correlated RPM votes,
cold GPU power, A418 command hardware, inherited SMMU routes, both context banks,
microcode loading, fences and exact memory readback in one bounded caller.
It is exercised by an independent host device model, **not on the Lumia**.
The default IOKit driver and installed EFI applications do not call it.

No native GPU power-on, completed GPU command, rendered frame, measured frame rate,
or iOS boot on this handset is established by these changes. A command-processor
memory write would be an intermediate milestone, not shader rendering or 60 FPS.

## Batch operations

`drivers/NativeAdreno418/GpuBatch.{h,cpp}` performs the complete sequence:

1. Reject missing qualifications, invalid budgets, reused batch objects, exhausted
   message-ID ranges, overlapping allocations and nonempty translation roots before
   touching the channel or hardware. The caller must reserve a fresh message epoch.
2. Build owned 4 KiB LPAE tables: read-only command ring, writable fence and target.
   Table memory is never GPU-mapped. CPU, physical and IOVA aliases are rejected.
3. Observe, claim and natively open the measured RPM message-RAM channel. The
   2048-byte allocation contains **two 1024-byte FIFOs**, not one 2048-byte direction.
4. Obtain all qualified supply/configuration-bus/memory-bus/graphics votes, in order,
   with correlated ACKs before clearing software collapse or enabling branches.
5. Bring up the two OxiLi domains, assert the GX core clock's FF/memory retention
   bits (14/13), then enable the three branches with preserved control bits and bounded
   polls. Only a cold, software-controlled, exclusively owned GPU qualifies. Restore
   the inherited retention bits only after both domains have demonstrably collapsed.
6. Hold the command processor, power A418 SP/TP with a bounded wait, configure the
   A430-derived VBIF settings, A418 bootstrap workaround and internal clock controls.
   Register protection covers privileged blocks, XPU-sensitive ranges and the SMMU.
   GPU IRQs stay masked. GMEM, shaders, display ownership and scanout are not configured.
7. Read and qualify existing SMMU routing for user SID 0/context 0 and privileged
   SID 1/context 1. Bypass, masked/ambiguous matches, aliases onto these banks, existing
   enabled banks, faults, unknown-stream bypass and unsupported geometry are refused.
   The batch does **not** rewrite global stream routing, security or TrustZone state.
8. Program both disabled banks with cleaned tables. Translate every ring page for
   reads, and the fence/target for reads and writes, using unprivileged ATS for the user
   context and privileged ATS for the privileged context. Verify PAR and fault state.
9. Load qualified PM4/PFP images, submit ME initialization and require its exact fence
   and drained ring. Submit two complementary CP_MEM_WRITE payloads into a CPU-poisoned
   owned target, requiring each exact fence, full data readback and untouched tail guards.
10. Halt CP, revalidate routing, disable and synchronize both banks, collapse SP/TP,
    disable branches/domains, restore leased votes to the recorded baseline in reverse
    order, close the channel, and retire the native memory binding.

`dmaReusable` becomes true only after the entire sequence succeeds. After claim, any
failure pins the caller's resources and preserves the failing stage and component
results; it does not free uncertain DMA, retry a published RPM transaction or pretend
that rollback succeeded. Retirement never releases RPM's physical message RAM.
The caller must serialize the batch, keep the GPU interrupt controlled at the GIC,
and retain every buffer, callback context and object until reset or separately verified
teardown. Post-exit callbacks must not invoke firmware boot services. There is no
asynchronous owner lock.

## Leased power votes

`RpmVotes` combines a qualified inherited baseline with up to eight owners for each
of eight resources, taking the componentwise maximum only for explicitly qualified
scalar-max keys. Key order is normalized. Unchanged aggregates require no RPM request.
Existing live owners may raise but not lower their lease through acquisition; lowering
requires a quiescent release. A release cannot undercut other owners or the baseline.

Lease/aggregate updates commit only after the matching ACK. An uncertain proposal is
retained separately and poisons the bank. Backpressure before publication leaves it
retryable with a fresh ID. Neither ACKs nor these software records measure a rail or
clock frequency. The baseline qualification flags are caller contracts, **not queries**
of the RPM state and not permission to guess zero baselines.

`PowerController` refuses unknown profiles, existing ownership, warm hardware,
hardware-controlled GDSCs and rates above the qualified board cap. A failure retains
votes rather than dropping power under a potentially active consumer. Its cold sequence
and resource list still need qualification against this Lumia; the LG regulator profile
is not installed as a Lumia profile.

## Independent validation

`Test/check-gpu-batch.cpp` models separate CPU/device cache views, native SMEM address
translation, an independently decoded SMD/RPM peer, MMSS transitions, an independent
LPAE walker, SMMU ATS and an independent PM4 interpreter. It does not replace RPM with
a success-returning callback or call the production command encoder to decode packets.
Supply and memory-bus resources in this model are explicitly **synthetic test resources**.
They are not a handset power profile. Firmware words in the host test are synthetic too.

The model covers the normal sequence, missing qualifications, allocation aliases,
lost/stale/malformed/error ACKs, blocked domains/clocks/SP/TP, bad protection readback,
bypass/aliased/live routes, context/TLB/translation faults including write permission,
stalled commands, wrong fences, corrupt data/guards, failed quiescence and teardown,
and every individual MMIO write failure in the successful trace. ASan/UBSan and strict
conversion warnings are enabled. Separate leased-vote tests cover multi-owner floors,
nonzero baselines, key normalization, bounded owners, live lease decreases and uncertain
publication/release. These tests are simulation evidence only.

`scripts/test-gpu.sh` runs both suites with existing core/transport tests. The kernel
archive source list includes all new cores. `scripts/build-probe.sh` also cross-compiles
them for ARM64 COFF and links `gpu-batch-link-check.efi` without dead stripping to catch
missing freestanding symbols. That **link-only image uses the existing RPM diagnostic
entry**, does not execute the GPU batch, and stays outside the ESP installation tree.
Do not install it as a GPU test. The existing four EFI applications retain their entries.

## Remaining hardware qualification

The user-reported native channel handshake is still the last handset execution
milestone. This batch does not fabricate the missing inputs:

- Exact Lumia supply and bus dependencies, valid voltage/corner values and a retained
  APPS RPM baseline/ownership record across ExitBootServices.
- Qualified native mappings and allocation/cache-maintenance callbacks for GPU MMIO,
  SMMU registers, translation tables and DMA buffers.
- Actual inherited SMMU stream/security configuration. Unsupported state needs a
  separately verified ownership/security handover, not a guessed SCM call.

The upstream Linux v6.12 Lumia board description adds an important distinction
from the LG example: its `vdd_gfx` is PMI8994's directly controlled SPMI `s2`,
with a fixed 980000-microvolt constraint. The PMI8994 node is on SPMI USID 3,
and the regulator driver's `s2` control peripheral starts at `0x1700`.
These are static board-description/register facts, **not a measurement of this
phone's regulator or permission to write it**. They do not establish that an RPM
`smpb` supply lease is the correct Lumia supply path. The current all-RPM power
plan must not be activated by filling its synthetic supply slot with LG values.
A real board adapter needs to qualify and retain the actual direct supply state,
in addition to the remote clock/bus contracts.

The phone's existing Windows PEP driver was copied from its **read-only** volume for
local research. Its static table independently confirms `/clk/oxili -> clk2, ID 1`,
with KHz units. It does not reveal live RPM votes or establish a complete regulator
sequence. Its GX callback and lists also independently identify the core-clock
FF/memory retention masks used here; the callback's original unbounded power wait
is not reproduced. The native cold shutdown deliberately restores inherited bits
after verified collapse, rather than claiming to reproduce the Windows clock-policy
state machine. See [the derived PEP observations](hardware/2026-10-27-pep-gpu-reference.json).
Vendor binaries and private disassembly/decompilation outputs are not committed.

After an actual completed command, shader/texture submission, render-target ownership,
compositor integration and scanout timing remain separate work. A successful CP fill
must not be presented as a GPU-accelerated wallpaper or an iOS graphics driver.

## Register-fact sources

- [Pinned Qualcomm MSM8992 A4xx implementation](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/gpu/msm/adreno_a4xx.c):
  A418 selects A430 VBIF values and uses the bit-14 bootstrap workaround, despite
  loading A420 microcode. The SP/TP status is bit 20; the original unbounded wait is
  not reproduced. The 70 effective clock register/value pairs are numerical facts,
  independently applied by the new bounded caller.
- [Pinned A4xx register definitions](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/gpu/msm/a4xx_reg.h):
  offsets are dword indices. Protection slots 0 through 14 are contiguous at
  `0x240..0x24e`; `CP_PROTECT_REG_10` names hexadecimal slot 0x10, not decimal slot 10.
- [Linux ARM SMMU register definitions](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu.h):
  ID/geometry, SMR/S2CR/CBAR and privileged/unprivileged read/write ATS registers.
- [Linux v6.12 Lumia board supply description](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/msm8994-msft-lumia-octagon.dtsi),
  [PMI8994 SPMI node](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/pmi8994.dtsi)
  and [SPMI regulator definitions](https://github.com/torvalds/linux/blob/v6.12/drivers/regulator/qcom_spmi-regulator.c):
  the board's fixed `vdd_gfx` constraint, USID and control-peripheral address;
  not evidence of a successful native GPU run on this handset.

These references explain the software's register facts, not validation of their
effects on this handset. The new public platform contracts remain unfulfilled until
actual board evidence is recorded and a real caller enforces them.
