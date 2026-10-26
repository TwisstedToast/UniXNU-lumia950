# Native GICv2 integration

This adds a shared GICv2 register engine and an integration patch for the native
UniXNU ArmCpu build. It is separate from `ci/hackios-gic-v2.patch`.

## Sources and build path

- ArmCpu repository: [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu).
- Qualified revision: `9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b`.
- Input `ArmCpu.cpp` SHA-256:
  `3c87b3bd617be5582a6a24ff797c359497ee9feaa702ee883ecee4270db21c9f`.
- Generated source SHA-256:
  `6c0a3c94872e8070669d6f3a53a42fbfdb065aa16cd4d9644c1caa49ce5c3a9d`.

`build-arm-driver.py ArmCpu` runs `prepare-armcpu.py`, applies
`drivers/NativeGicV2/ArmCpu.patch` in a temporary directory, verifies the output
hash, and compiles the generated source plus `GicV2Core.cpp` into `libArmCpu.a`.
The upstream checkout is not modified. The patch lives inside the linked driver
module, so an assembled build tree does not need a `ci/` directory. A manifest
records both source identities and the core/header hashes. Different upstream contents must be reviewed,
not patched by fuzzy matching. Existing board archive selection still builds ArmCpu.

## Implemented behavior

- Parse the complete NUL-separated compatible property; refuse malformed or
  conflicting generation strings. Recognize `qcom,msm-qgic2` and `arm,gic-2`.
- Map 4 KiB GICv2 distributor and CPU-interface windows; check PIDR2 at offset
  `0xfe8`. Preserve the GICv3 path and its larger redistributor requirements.
- Quiesce the virtual timer before local interrupt setup. Configure SGI 0 and
  virtual-timer PPI 27 through banked distributor registers, not redistributors.
- Detect the security register view with a restored SGI-group probe. Use Group 0
  in the secure view or the non-secure Group 1 alias. Do not program group fields
  when security extensions are absent. Refuse inaccessible priorities/enables.
- Derive the boot interface's one-hot target mask from banked ITARGETSR reads.
  Do not guess it from MPIDR, logical core number, or cluster numbering.
- GICC PMR/BPR setup and EOI mode 0. Decode INTID from the low ten bits but retain
  the entire IAR token, including source CPU bits, for EOIR. Never EOI spurious
  tokens 1020–1023.
- SPI priority, edge/level, target, pending/active clearing and separate unmask
  after handler publication. Mask and count unknown peripheral interrupts.
- Memory-mapped SGIR self-IPI, GICC interrupt dispatch, diagnostic snapshots and
  quiesce. GICv2 diagnostic calls do not fall through to ICC system-register code.
- Boot CPU only. GICv2 secondary registration/startup and remote IPIs are refused;
  the existing GICv3 SMP implementation is not being qualified for MSM8992.
  The GICv2 IOCPU interrupt controller has one CPU slot, and the board's first-boot
  arguments include `cpus=1` so it does not wait for unstarted secondary CPUs.

## Validation

```sh
bash scripts/test-gicv2.sh
python3 scripts/test-armcpu-patch.py /path/to/pinned/ArmCpu.cpp
bash scripts/build-gicv2-test.sh
python3 scripts/test-gicv2-qemu.py build/gicv2-qemu/gicv2-test.elf
```

Host tests use ASan/UBSan with fake security views, register rejection, nontrivial
CPU target masks, SPI fields, exact raw EOI tokens, spurious IDs, compatible-list
mutations and window bounds. Preparation tests verify both source hashes,
determinism, input immutability, changed-source/patch refusal and assembled-tree
layout. Neither compiles IOKit.

The standalone AArch64 test uses the same `GicV2Core.cpp`. QEMU has two CPU
interfaces so banked target discovery is meaningful; only CPU 0 executes the
test and CPU 1 is parked. IRQs enter an actual AArch64 exception vector. Two
rounds each of timer PPI 27, SGI 0 and software-pended SPI 40 prove asynchronous
delivery and EOI/rearm. The test does not poll IAR from its main loop.

Both security-disabled and security-enabled runs pass on QEMU 10.2. In the latter,
the small EL3 harness assigns Group 1 sources and a usable priority mask before
entering non-secure EL1; SPI 41 is deliberately retained as secure and refused by
the core. These are explicit simulated-firmware inputs, not a TrustZone bypass or
evidence that the Lumia firmware supplies them. Logs are in
`out/gicv2-validation/secure-off.log` and `secure-on.log`.

An initial emulation run exposed a real PMR handoff requirement: non-secure EL1
cannot raise a secure-owned low priority mask. The core checks readback and
refuses that state. It does not pretend that a successful MMIO write changed it.
A third QEMU run deliberately leaves that mask inaccessible and verifies an
explicit refusal rather than interrupt-enable success.

## Remaining gates

The generated ArmCpu IOKit source has not been compiled with the selected
XNU/iPhoneOS SDK recipe. The Mac has no configured iPhoneOS SDK, and no new
kernel artifact was produced. Core cross-compilation and QEMU do not qualify
the XNU ABI, timer/scheduler integration, boot memory layout or loader.

On the Lumia, qualify the inherited GIC security ownership, target mask, PMR,
PPI 27 delivery, `rtclock_intr`, self-IPI and a real device SPI. External-device
register-map/clock qualification remains mandatory. The phone's staged GPU
preflight is unchanged; this work does not install a new fallback or kernel.

## Primary controller reference

Reviewed against Linux v6.12's [GICv2 driver](https://github.com/torvalds/linux/blob/v6.12/drivers/irqchip/irq-gic.c):
banked CPU target discovery, interrupt groups, raw SGI acknowledgement, MMIO CPU
interface and peripheral routing. The native wrapper is based on the pinned
ArmCpu source above, with board addresses still supplied by the Lumia ADT.
