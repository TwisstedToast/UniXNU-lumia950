#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
EDK2=${UNIXNU_EDK2:-}
OUT=${1:-$ROOT/out/probe}
CLANG=${PROBE_CLANG:-clang}
LINKER=${PROBE_LINKER:-lld-link}
PYTHON=${PYTHON:-python3}
ENTRY=${PROBE_ENTRY:-LumiaProbe}

case "$ENTRY" in
  LumiaProbe|LumiaHandoff|LumiaGpuProbe) ;;
  *) echo "error: PROBE_ENTRY must be LumiaProbe, LumiaHandoff or LumiaGpuProbe" >&2; exit 1 ;;
esac

if [ -z "$EDK2" ] || [ ! -f "$EDK2/MdePkg/Include/Uefi.h" ]; then
  echo "error: set UNIXNU_EDK2 to an EDK II checkout containing MdePkg/Include/Uefi.h" >&2
  exit 1
fi
mkdir -p "$OUT/objects" "$OUT/esp/EFI/LumiaProbe" "$OUT/esp/EFI/BOOT"

for component in ProbeCore FramebufferCore LumiaProbe LumiaHandoff; do
  "$CLANG" --target=aarch64-pc-windows-msvc -std=c11 -Os \
    -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
    -mno-stack-arg-probe -mgeneral-regs-only -mstrict-align \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -Wall -Wextra -Werror \
    -I "$EDK2/MdePkg/Include" -I "$EDK2/MdePkg/Include/AArch64" \
    -I "$ROOT/tools/uefi-probe" \
    -c "$ROOT/tools/uefi-probe/$component.c" -o "$OUT/objects/$component.obj"
done

for component in AdrenoCore IommuCore CommandStream GpuIo LpaeCore SmmuContext Submission PlatformCheck RpmCore SmdPipe; do
  "$CLANG" --target=aarch64-pc-windows-msvc -std=c++17 -Os \
    -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
    -fno-exceptions -fno-rtti -mno-stack-arg-probe -mgeneral-regs-only -mstrict-align \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror \
    -I "$ROOT/drivers/NativeAdreno418" \
    -c "$ROOT/drivers/NativeAdreno418/$component.cpp" -o "$OUT/objects/$component.obj"
done
for component in LumiaGpuProbe ReportStore FirmwareClock FirmwareSmem; do
  "$CLANG" --target=aarch64-pc-windows-msvc -std=c++17 -Os \
    -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
    -fno-exceptions -fno-rtti -mno-stack-arg-probe -mgeneral-regs-only -mstrict-align \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror \
    -I "$EDK2/MdePkg/Include" -I "$EDK2/MdePkg/Include/AArch64" \
    -I "$ROOT/tools/uefi-probe" -I "$ROOT/drivers/NativeAdreno418" \
    -c "$ROOT/tools/uefi-probe/$component.cpp" -o "$OUT/objects/$component.obj"
done

for application in LumiaProbe LumiaHandoff LumiaGpuProbe; do
  application_objects=("$OUT/objects/ProbeCore.obj")
  if [ "$application" = LumiaHandoff ]; then
    application_objects+=("$OUT/objects/FramebufferCore.obj")
  fi
  if [ "$application" = LumiaGpuProbe ]; then
    application_objects+=("$OUT/objects/GpuIo.obj" "$OUT/objects/PlatformCheck.obj")
    application_objects+=("$OUT/objects/ReportStore.obj" "$OUT/objects/FirmwareClock.obj")
    application_objects+=("$OUT/objects/FirmwareSmem.obj")
  fi
  "$LINKER" /machine:arm64 /subsystem:efi_application /entry:UefiMain \
    /nodefaultlib /base:0x400000 /dynamicbase /fixed:no /timestamp:0 \
    /opt:ref /opt:icf \
    "/out:$OUT/esp/EFI/LumiaProbe/$application.efi" \
    "$OUT/objects/$application.obj" "${application_objects[@]}"
  "$PYTHON" "$ROOT/scripts/check-probe-image.py" "$OUT/esp/EFI/LumiaProbe/$application.efi"
done
cp "$OUT/esp/EFI/LumiaProbe/$ENTRY.efi" "$OUT/esp/EFI/BOOT/BOOTAA64.EFI"
echo "EFI fallback application: $ENTRY"
echo "No files were copied to the phone."
