#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/build/storage-tests}
CXX=${CXX:-c++}
mkdir -p "$OUT"
for test_name in check-sdhci-core check-polling-host; do
  "$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$ROOT/drivers/NativeSdhci" \
    "$ROOT/drivers/NativeSdhci/SdhciCore.cpp" \
    "$ROOT/drivers/NativeSdhci/PollingHost.cpp" \
    "$ROOT/drivers/NativeSdhci/Test/$test_name.cpp" -o "$OUT/$test_name"
  "$OUT/$test_name"
done
