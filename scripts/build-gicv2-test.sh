#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/build/gicv2-qemu}
CC=${CC:-clang}
CXX=${CXX:-clang++}
LD=${LD:-ld.lld}
mkdir -p "$OUT"
flags=(-target aarch64-none-elf -mcpu=cortex-a53 -mgeneral-regs-only -ffreestanding
       -fno-builtin -fno-stack-protector -O2 -g -Wall -Wextra -Werror)
"$CC" "${flags[@]}" -c "$ROOT/tools/gicv2-qemu/entry.S" -o "$OUT/entry.o"
"$CXX" "${flags[@]}" -std=c++17 -fno-exceptions -fno-rtti \
  -c "$ROOT/drivers/NativeGicV2/GicV2Core.cpp" -o "$OUT/GicV2Core.o"
"$CXX" "${flags[@]}" -std=c++17 -fno-exceptions -fno-rtti \
  -c "$ROOT/tools/gicv2-qemu/test.cpp" -o "$OUT/test.o"
"$LD" -T "$ROOT/tools/gicv2-qemu/link.ld" "$OUT/entry.o" "$OUT/GicV2Core.o" \
  "$OUT/test.o" -o "$OUT/gicv2-test.elf"
"$CC" "${flags[@]}" -DGIC_TEST_BLOCKED_PMR -c "$ROOT/tools/gicv2-qemu/entry.S" -o "$OUT/entry-blocked-pmr.o"
"$LD" -T "$ROOT/tools/gicv2-qemu/link.ld" "$OUT/entry-blocked-pmr.o" "$OUT/GicV2Core.o" \
  "$OUT/test.o" -o "$OUT/gicv2-test-blocked-pmr.elf"
printf '%s\n' "$OUT/gicv2-test.elf"
