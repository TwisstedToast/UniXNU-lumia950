#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/build/gicv2-tests}
CXX=${CXX:-c++}
mkdir -p "$OUT"
"$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$ROOT/drivers/NativeGicV2/GicV2Core.cpp" \
  "$ROOT/drivers/NativeGicV2/Test/check-gicv2.cpp" -o "$OUT/check-gicv2"
"$OUT/check-gicv2"
