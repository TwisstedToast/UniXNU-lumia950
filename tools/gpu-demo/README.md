# Real GPU animation: Windows hardware baseline

This optional ARM64 Windows application uses the Lumia's **existing Adreno 418
D3D11 driver**. It is not an EFI application, a native XNU driver or an iOS boot.
It does not resolve native power, SMMU, shader ABI, IOSurface or compositor work.
Its purpose is to get an actual hardware-rendered animation and a measured
performance baseline on this phone without waiting for that whole port.

## Run on the Lumia

1. Boot the phone's existing Windows-on-ARM desktop installation, not the RPM test.
2. Copy the demo directory from the SSD to a writable folder in Windows and open
   `LumiaGpuDemo.exe`. Alternatively run it directly from a writable attached drive.
3. It animates fullscreen for 30 seconds. Tap the screen after two seconds, or
   press Escape, to exit earlier. `--seconds=60` selects a 60-second run (5–300).
4. Keep the window foreground. Do not use Remote Desktop or minimize the window
   while measuring. A screenshot and reports are saved beside the EXE, falling
   back to `%TEMP%` if that directory is not writable. The exit dialog gives the
   exact `LumiaGpuDemo-<UTC>-<PID>` directory.

Nothing installs a driver, changes boot files, modifies partitions or adjusts
voltages/clocks directly. The existing Windows driver owns the GPU. No firmware
or proprietary binaries are included. The app does not automatically launch at
boot. If the existing Windows installation does not boot, that remains a separate
prerequisite; this EXE cannot run from the UEFI menu or USB mass-storage mode.

## What is rendered

The GPU runs a vertex and pixel shader on a fullscreen triangle. Time-dependent
wallpaper waves and a shaded rotating-ribbon orb are calculated in the pixel
shader on every frame. The CPU supplies the time and a small uniform buffer; it
does not upload animation images. GDI creates the measurement text texture once
per second; that text texture is not the animation. No WARP, reference-device,
software rasterizer or CPU fallback is attempted. By default, only a non-software
Qualcomm adapter whose description names Adreno 418 is accepted.

The initial target is VSync-limited 60 FPS, **not a promised or pre-recorded result**.
Resolution comes from the actual Windows client/backbuffer, not the UEFI mode or
an assumed 950 XL panel. The driver chooses the display refresh mode and GPU rate.

## Measurements

- `DXGI display FPS` derives from `GetFrameStatistics` present counts and QPC
  synchronization times over at least two seconds. Refresh rate is calculated
  separately from refresh counts. A 60 Hz refresh is not reported as 60 rendered FPS.
  If the driver/presentation model does not supply usable statistics, it explicitly
  shows **unavailable**, not a fabricated 60. Disjoint or stale samples reset it.
- `Present loop` measures successful `Present(1, 0)` calls. It is clearly labeled
  separately and is not proof that the screen displayed every submitted image.
- `GPU ms` measures the shader draw using timestamp/disjoint queries. Queries are
  read asynchronously without flushing, never by substituting CPU elapsed time.
  A completed run requires at least one valid GPU timestamp sample. These times
  exclude display scanout, CPU HUD preparation and presentation overhead.
- `frames.csv` retains counters, HRESULTs, raw QPC values and latest GPU timing.
  `report.txt` includes adapter identity, software flag, feature level, dimensions,
  presentation model, errors and valid measurement counts.
- `gpu-frame.bmp` is copied back from the real GPU render target after four seconds,
  before presentation. The one-time readback can interrupt that measurement window;
  use later windows for steady-state performance. It is not a photograph or an
  independent optical scanout/tearing measurement.

No measured handset FPS is claimed until this application actually runs there and
its reports are retrieved. A fast CPU loop or successful build alone is not a pass.

## Build and check

On macOS/Linux with Zig 0.15.2:

```
python3 scripts/build-windows-gpu-demo.py --out /path/on/ssd/gpu-demo
bash scripts/test-gpu-demo.sh /tmp/gpu-demo-tests
```

The build embeds `Animation.hlsl`, links only Windows system DLLs, checks the
executable's ARM64 PE32+/Windows GUI contract and records source/binary hashes.
It never copies anything to the phone. The linker works in the system temporary
directory, then the builder streams, syncs and verifies the final EXE on the SSD.
This avoids memory-mapped linker output on third-party NTFS filesystems. Zig caches
also use the temporary directory by default; `ZIG_GLOBAL_CACHE_DIR` and
`ZIG_LOCAL_CACHE_DIR` can select another reliable native filesystem. Allow a few
hundred MB for compiler caches, independently of the small final application.

In an MSVC native or cross-build developer prompt:

```
python scripts/build-windows-gpu-demo.py --compiler msvc --arch arm64 --out out/gpu-demo
```

Use the matching compiler environment for `--arch`. CI also builds an x64 copy
and runs `--shader-check`, which compiles the exact embedded HLSL with Microsoft's
D3D compiler **without creating a GPU device**. That checks shaders, not handset
rendering. `--allow-other-hardware` is an explicit developer-only switch for a
different physical GPU; those results are labeled non-Lumia and do not qualify
the phone. CPU/WARP adapters remain forbidden even with that switch.

The native iOS path remains in `drivers/NativeAdreno418`; see
`docs/native-gpu-batch.md` for its actual qualification gaps.

## Primary API references

- [DXGI frame statistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics)
  distinguishes displayed image counts, v-blanks and QPC synchronization times.
- [GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics)
  documents flip-model support and windowed blt limitations.
- [GPU timestamp/disjoint data](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_timestamp_disjoint)
  explains the hardware counter frequency and invalidation of disjoint intervals.
