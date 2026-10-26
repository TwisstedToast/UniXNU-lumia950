#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/build/report-tests}
EDK2=${UNIXNU_EDK2:-}
CXX=${CXX:-c++}
if [ ! -f "$EDK2/MdePkg/Include/Uefi.h" ]; then
  echo "set UNIXNU_EDK2 to the EDK II headers used for the EFI build" >&2
  exit 1
fi
case "$(uname -m)" in
  arm64|aarch64) architecture=AArch64 ;;
  x86_64|amd64) architecture=X64 ;;
  *) echo "unsupported native UEFI test architecture" >&2; exit 1 ;;
esac
mkdir -p "$OUT"
"$CXX" -std=c++17 -fshort-wchar -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$EDK2/MdePkg/Include" -I "$EDK2/MdePkg/Include/$architecture" \
  "$ROOT/tools/uefi-probe/ReportStore.cpp" "$ROOT/tools/uefi-probe/FirmwareClock.cpp" \
  "$ROOT/tools/uefi-probe/Test/check-report-store.cpp" -o "$OUT/check-report-store"
"$OUT/check-report-store"
"$CXX" -std=c++17 -fshort-wchar -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$EDK2/MdePkg/Include" -I "$EDK2/MdePkg/Include/$architecture" \
  "$ROOT/tools/uefi-probe/FirmwareSmem.cpp" \
  "$ROOT/tools/uefi-probe/Test/check-firmware-smem.cpp" -o "$OUT/check-firmware-smem"
"$OUT/check-firmware-smem"
