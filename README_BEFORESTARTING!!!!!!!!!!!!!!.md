# STOP. READ THIS BEFORE STARTING ANYTHING.

## The GitHub Action is the compiler. Not this machine.

`scripts/build-arm-driver.py` replays the compile command XNU recorded and needs Xcode and
the iPhoneOS SDK. This Mac has Command Line Tools but no verified iPhoneOS SDK. The
standalone EFI diagnostics and freestanding backend cross-compiles are separate. So:

> **A driver that has only been edited is not compiled. A driver is compiled when the
> `kernel` job on GitHub Actions says so. Push, dispatch the job, read the log.**

Do not say a driver works, or even builds, because `py_compile`, `g++` on the host test and
`bash -n` passed. Those test the *core* and the *scripts*. They do not compile
`NativeSdhci.cpp`, `NativeAdreno418.cpp` or anything else that includes `<IOKit/...>`.

## Run it

```sh
# The job that compiles every driver listed in [kernel] archives of board.toml:
gh workflow run build.yml -f target=kernel --repo TwisstedToast/UniXNU-lumia950

# See it, follow it, get the artifact:
gh run list --workflow build.yml --repo TwisstedToast/UniXNU-lumia950
gh run watch <run-id> --repo TwisstedToast/UniXNU-lumia950
gh run download <run-id> -n lumia950-kernel --repo TwisstedToast/UniXNU-lumia950
```

`target` is `payload`, `kernel` or `both`; the default is `payload`, so **name `kernel`
explicitly** or you get the EFI build and no driver compile at all.

CI pins UniXNU to `1a01c2afa197d975d04da892325c56b6b14aaecb`, the base of the
successful 2026-10-24 kernel build. Its 16.x overlay is not qualified against a moving
upstream `main`; changing that pin requires revalidating the patch and driver ABI.

| Job | Runs when | What it is the only proof of | Artifact |
| --- | --- | --- | --- |
| `validate` | every push to `main`, every PR, **and every dispatch** | board/defconfig parsing, storage/GIC/GPU/display/probe core tests, addon and ADT checks, board acceptance and the incomplete-board guard; not IOKit compilation | none |
| `gicv2-emulation` | every push, PR and dispatch | pinned ArmCpu patch preparation and standalone asynchronous timer/SGI/SPI exceptions in QEMU; not XNU or handset validation | `lumia950-gicv2-core-emulation` |
| `efi-preflight` | every push, PR and dispatch | AArch64 EFI image build, mock report/clock tests, non-Lumia MMIO refusal and actual two-boot report persistence in QEMU; not XNU or handset GPU execution | `lumia950-efi-preflight` |
| `payload` | dispatch with `target=payload` or `both` | `UniXNU.efi`, `DarwinBoot.efi` and the staged `out/payload` tree | `lumia950-payload` |
| `kernel` | dispatch with `target=kernel` or `both` | **every driver compiles against the recorded XNU ABI with `-Werror`**, the kernel links, and the pinned 16.7.16 kernelcache merges | `lumia950-kernel` |

## Run it even when you are not sure you need to

If a change *might* have broken compilation, **just dispatch the job**. Actions minutes are
free on a public repository, the run is the cheapest possible smoke test, and a red log that
names a file and a line is worth more than an hour of reading the diff. There is no quota to
spend here and no reason to hoard a run. Same for the payload job: if the menu or the loader
changed, build it and look at the artifact.

Do not ask "should I run CI?" — run it, then read the log.

## The traps, in the order people fall into them

1. **A new driver is invisible to CI until it is in `[kernel] archives` in `board.toml`.**
   `drivers/Foo/` existing is not enough. Add `"build-arm-driver.py Foo"` and the matching
   `COMPONENTS` entry in `scripts/build-arm-driver.py`, or the job happily compiles everything
   except your new file and goes green.
2. **A green `validate` is not a compiled driver.** It never includes `<IOKit/...>`. Read the
   `kernel` job's log for your class name, not the checkmark.
3. **A new ADT node has to pass `check-adt.py` first.** `validate` runs the generator and the
   checker, including the checker's `--self-test`; a node the checker rejects makes the
   `kernel` job pointless because the phone would never get the tree.
4. **The archive is built, not linked.** `-Wl,-force_load` and the `KernelConfigTables.cpp`
   personality are separate steps (README, "Linking it into the kernel"). A compiled driver
   with no personality is never matched, and nothing on this machine can tell you that.
5. **The `kernel` job is long (macOS + Xcode).** Dispatch it as soon as a driver change is
   ready, do not stack five changes onto one run whose failure will not name the culprit.
6. **A backend in the archive is not a hardware result.** `LpaeCore`, `SmmuContext`, and
   `Submission` now implement mapping, context registers, and CP submission. The default
   driver does not activate them. ASan/UBSan tests use simulated MMIO; the EFI build
   cross-compiles those sources but its preflight never submits GPU work. Neither proves
   on-phone acceleration. See `docs/gpu-bringup.md` for the hardware preconditions.
7. **The display driver publishes no framebuffer, on purpose.** `NativeMdss` runs at rung 1:
   it maps and reads. A green `validate` says its arithmetic is right, not that anything is
   on the panel. Rung 4 is where a picture could appear and rung 5 where iOS could draw; the
   DSI PHY's PLL table and the panel's init sequence are still absent, and they are not
   things to guess at.
8. **A window has to contain the registers read through it.** The MMSS window was declared
   4 KiB while the GDSCs sit at `0x4024`; the read went past the mapping. Both core tests now
   assert the offsets are inside their windows — do the same for any node you add.
9. **The native GICv2 patch is not the older HackiOS patch.** The ArmCpu builder now
   generates a qualified source copy and includes `GicV2Core.cpp`. Source hash drift
   must fail, not silently compile an unpatched GICv3-only driver. GICv2 secondary
   startup is refused. The standalone IRQ test validates the shared core, not the
   generated IOKit source. Read `docs/native-gicv2.md` before changing that boundary.
10. **Storage completion is asynchronous even with polled PIO.** `PollingHost.cpp`
    waits for the final transfer event and checks controller/R1 errors. Do not put
    the old immediate-completion sample or raw CSD-register ordering back into
    `NativeSdhci.cpp`. Current wrapper edits still need a new SDK compile.

## Resolution and screenshots

The native panel is **1440x2560**, but the 2026-10-26 on-device EFI probe measured an active
GOP mode of **1080x1920**, with a 1080-pixel stride and a BGR framebuffer at 0x00400000.
iOS takes its boot geometry from `PE_current_console`, so the loader must use the actual GOP
mode rather than substituting the native panel size. Select the QEMU display size explicitly
for the mode under test. For a raw capture from the measured firmware mode, run
`python3 scripts/check-screenshot.py FILE --panel 1080x1920`. A camera photograph is not a
raw framebuffer capture and its image dimensions do not validate GOP geometry.

## The short version

```
edited a driver  ->  push  ->  gh workflow run build.yml -f target=kernel  ->  read the log
edited the menu  ->  push  ->  gh workflow run build.yml -f target=payload ->  take the artifact
edited a script  ->  push  ->  validate is enough
```

The Action verifies the XNU/IOKit compile. Local host tests, standalone EFI builds,
emulator results, and handset photographs are separate evidence with separate limits.
