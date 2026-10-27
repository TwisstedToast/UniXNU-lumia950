#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/out/gpu-demo-tests}
mkdir -p "$OUT"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
  -fsanitize=address,undefined -I "$ROOT/tools/gpu-demo" \
  "$ROOT/tools/gpu-demo/Test/check-timing.cpp" -o "$OUT/check-timing"
"$OUT/check-timing"
