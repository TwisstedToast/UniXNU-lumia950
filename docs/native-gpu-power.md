# Native GPU power path: RPM/SMD

The target remains an on-handset, GPU-rendered image and eventually the iOS compositor.
A Windows Direct3D demo, a CPU-written EFI animation, and a 60 Hz panel setting do not
demonstrate that native target. No native GPU-rendered frame or 60 FPS result exists yet.

## Concrete dependency found

The MSM8992 graphics branch is not its own frequency source. The pinned Qualcomm
`clock-mmss-8992.c` attaches `oxili_gfx3d_clk` to a parent supplied by RPM. Its platform
device-tree wiring resolves that parent through `clk_oxili_gfx3d_clk_src`. The RPM clock
definition identifies **resource type `0x326b6c63` (`clk2`), resource ID 1**, with a
`0x007a484b` (`KHz`) scalar rate key. Hertz must be rounded upward to kilohertz.
MMSS NOC AHB is a different resource: **`clk1`, ID 3**. A successful ACK acknowledges
the request, not a measured clock rate or a working GPU.

The reference MSM8992 GDSC description also gives GX a parent supply,
`pmi8994_s2_corner`. Thus only clearing a GDSCR collapse bit is not a complete power-on
sequence. The LG/Qualcomm reference regulator's resource mapping and corner values
have **not** been qualified against this Lumia's actual PMIC configuration. They are
not silently installed as a handset power profile.

The published Lumia firmware's `rpm_send_data` calls a receive routine with unbounded
FIFO waits; it does not expose a checked, correlated ACK result to its caller. That
firmware's ExitBootServices handler also closes the SMD-RPM channel. Retaining a DXE
function pointer is therefore not a native post-handoff resource manager.

## Implemented components

`drivers/NativeAdreno418/RpmCore.{h,cpp}` provides independently implemented, allocation-free
request encoding, ACK decoding and a serialized resource transaction engine:

- Explicit active versus sleep sets, nonzero IDs, bounded scalar KVP arrays, duplicate
  key refusal, little-endian byte encoding and no uninitialized trailing payload.
- ACK service/length/TLV validation, exact `msg#` correlation, duplicate-ID refusal,
  bounded remote errors, stale-reply handling and refusal of sequence-number reuse.
- A shared send/receive poll budget, bounded requested delays, and no duplicate send
  after publication. ACK timeout, malformed response, peer reset or uncertain I/O
  poisons the session. A late reply cannot revive it or justify releasing resources.
- A backpressure timeout **before** publication can be retried with a new message ID;
  `WouldBlock` is contractually required to have no packet-publication side effects.

`drivers/NativeAdreno418/SmdPipe.{h,cpp}` implements the word-access SMD packet path:

- Two 44-byte control halves, separate transmit/receive FIFOs, 20-byte packet headers,
  bounds-checked word indices, power-of-two wrap, and reserved empty-ring space.
- Whole-packet publication and reception, no producer overwrite, no consumption of a
  partial header/payload, release/acquire barrier callbacks and remote notification.
- Exclusive channel ownership and uncached SMEM mappings are mandatory. The platform
  must validate/map the buffers and provide real ordered MMIO/barrier/IPC operations;
  cached mappings with no coherency implementation are refused.
- Adoption accepts only an already-open, empty channel. Native opening accepts only
  an exclusively owned, closed, quiescent channel and performs a bounded
  `CLOSED -> OPENING -> OPENED` polling handshake. Each endpoint resets its own indices.
  Resource traffic is disabled while opening. A failed handshake cannot be blindly
  retried on the same channel object.

Both components are single-caller internals, not a user-client ABI. The owner must
serialize callers/interrupt handlers and keep the mapped shared memory alive while the
channel state is uncertain. A genuine peer reset and re-established ownership are
required before constructing a replacement poisoned session; zeroing a struct alone
does not reset the remote processor or cancel its last resource vote.

## Validation

```sh
bash scripts/test-gpu.sh
UNIXNU_EDK2=/path/to/edk2 PROBE_ENTRY=LumiaGpuProbe \
    bash scripts/build-probe.sh out/gpu-rpm-crosscompile
```

The host suite uses ASan/UBSan. It includes an independent golden RPM message, malformed
and stale ACKs, bounded errors, backpressure and lost ACKs, sequence exhaustion, both
FIFO wrap directions, partial frames, producer/notification ordering, invalid indices,
failed publication, native reopen and multiple correlated transactions across a simulated
peer. Tests do not send real votes or power the handset GPU.

The EFI script cross-compiles these source files, but deliberately does **not** link them
into the diagnostic or enable new handset operations. The three EFI application hashes
remain unchanged. The XNU archive builder now includes them, but a new recorded-ABI
IOKit build has not run. No new phone application or firmware image is installed.

## Remaining work before an actual accelerated image

1. Discover and validate the handset's APPS-RPM SMEM channel, map its reserved buffers
   and IPC notification register, and bind the native callbacks after firmware handoff.
   Only the word-access layout is implemented here; alternate layouts must be refused.
2. Implement the shared resource-vote/lease policy, qualify the actual graphics supply
   profile and parent rates, then sequence supplies, CX/GX GDSCs and the three branches.
   Preserve other consumers' votes; do not interpret a transport ACK as exclusive supply
   ownership. Verify the measured rail/clock state before reading GPU/SMMU windows.
3. Establish A418 initialization, security/stream routing and owned SMMU mappings;
   prove a real GPU fence and poisoned-buffer readback before raster/blit work.
4. Add an actual A418 drawing/presentation path. Measure completed, changing GPU frames
   and display/vblank cadence over time before reporting 60 FPS. Host tests, GPU command
   processor memory writes and CPU framebuffer-loop counts are not that measurement.

## Primary references

- [MSM8992 MMSS graphics parent](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/clk/qcom/clock-mmss-8992.c)
- [RPM resource definitions](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/clk/qcom/clock-rpm-8994.c)
- [RPM Hz-to-kHz and peer-vote policy](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/drivers/clk/qcom/clock-rpm.c)
- [MSM8992 wiring and GX supply](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/arch/arm/boot/dts/qcom/msm8992.dtsi)
- [Reference regulator configuration, not a Lumia-qualified profile](https://github.com/LineageOS/android_kernel_lge_msm8992/blob/d25caae7761fc0dae85091fea7aa5a0a5006ea58/arch/arm/boot/dts/qcom/msm8992-regulator.dtsi)
- [RPM request and response protocol](https://github.com/torvalds/linux/blob/v6.12/drivers/soc/qcom/smd-rpm.c)
- [Lumia word-access SMD layout](https://github.com/WOA-Project/Lumia950XLPkg/blob/cfc68a4d19fc7e8645bff6d8fce7308ad8494a76/Driver/SmdRpmDxe/smd.h)
- [Reference SMD polling and ExitBootServices closure](https://github.com/WOA-Project/Lumia950XLPkg/blob/cfc68a4d19fc7e8645bff6d8fce7308ad8494a76/Driver/SmdRpmDxe/smd.c)
- [Reference firmware RPM send/receive](https://github.com/WOA-Project/Lumia950XLPkg/blob/cfc68a4d19fc7e8645bff6d8fce7308ad8494a76/Driver/SmdRpmDxe/rpm-smd.c)
