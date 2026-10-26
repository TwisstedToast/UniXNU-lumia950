#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/build/probe-tests}
CC=${CC:-cc}

mkdir -p "$OUT"
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tools/uefi-probe" \
  "$ROOT/tools/uefi-probe/ProbeCore.c" \
  "$ROOT/tools/uefi-probe/Test/check-probe-core.c" \
  -o "$OUT/check-probe-core"
"$OUT/check-probe-core"
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tools/uefi-probe" \
  "$ROOT/tools/uefi-probe/FramebufferCore.c" \
  "$ROOT/tools/uefi-probe/Test/check-framebuffer-core.c" \
  -o "$OUT/check-framebuffer-core"
"$OUT/check-framebuffer-core"
python3 "$ROOT/scripts/test-probe-fat.py"
