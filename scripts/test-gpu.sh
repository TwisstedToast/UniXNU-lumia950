#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/build/gpu-tests}
CXX=${CXX:-c++}
mkdir -p "$OUT"
sources=("$ROOT/drivers/NativeAdreno418/AdrenoCore.cpp"
         "$ROOT/drivers/NativeAdreno418/IommuCore.cpp"
         "$ROOT/drivers/NativeAdreno418/CommandStream.cpp"
         "$ROOT/drivers/NativeAdreno418/GpuIo.cpp"
         "$ROOT/drivers/NativeAdreno418/LpaeCore.cpp"
         "$ROOT/drivers/NativeAdreno418/SmmuContext.cpp"
         "$ROOT/drivers/NativeAdreno418/PlatformCheck.cpp"
         "$ROOT/drivers/NativeAdreno418/Submission.cpp")
for test_name in check-adreno-core check-gpu-backend; do
  "$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$ROOT/drivers/NativeAdreno418" "${sources[@]}" \
    "$ROOT/drivers/NativeAdreno418/Test/$test_name.cpp" -o "$OUT/$test_name"
  "$OUT/$test_name"
done
"${PYTHON:-python3}" "$ROOT/scripts/test-gpu-tools.py"
"${PYTHON:-python3}" "$ROOT/scripts/test-graphics-profile.py"
"${PYTHON:-python3}" "$ROOT/scripts/test-oem-scm-profile.py"
"$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/drivers/NativeAdreno418" \
  "$ROOT/drivers/NativeAdreno418/RpmCore.cpp" "$ROOT/drivers/NativeAdreno418/SmdPipe.cpp" \
  "$ROOT/drivers/NativeAdreno418/Test/check-rpm.cpp" -o "$OUT/check-rpm"
"$OUT/check-rpm"
"$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/drivers/NativeAdreno418" \
  "$ROOT/drivers/NativeAdreno418/RpmCore.cpp" "$ROOT/drivers/NativeAdreno418/SmdPipe.cpp" \
  "$ROOT/drivers/NativeAdreno418/SmdMemory.cpp" \
  "$ROOT/drivers/NativeAdreno418/Test/check-smd-memory.cpp" -o "$OUT/check-smd-memory"
"$OUT/check-smd-memory"
"$CXX" -std=c++17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -fsanitize=address,undefined \
  -I "$ROOT/drivers/NativeAdreno418" "${sources[@]}" \
  "$ROOT/drivers/NativeAdreno418/RpmCore.cpp" "$ROOT/drivers/NativeAdreno418/SmdPipe.cpp" \
  "$ROOT/drivers/NativeAdreno418/SmdMemory.cpp" "$ROOT/drivers/NativeAdreno418/RpmVotes.cpp" \
  "$ROOT/drivers/NativeAdreno418/PowerController.cpp" "$ROOT/drivers/NativeAdreno418/A418Init.cpp" \
  "$ROOT/drivers/NativeAdreno418/SmmuRoutes.cpp" "$ROOT/drivers/NativeAdreno418/GpuBatch.cpp" \
  "$ROOT/drivers/NativeAdreno418/Test/check-gpu-batch.cpp" -o "$OUT/check-gpu-batch"
"$OUT/check-gpu-batch"
"$CXX" -std=c++17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -fsanitize=address,undefined \
  -I "$ROOT/drivers/NativeAdreno418" \
  "$ROOT/drivers/NativeAdreno418/RpmCore.cpp" "$ROOT/drivers/NativeAdreno418/RpmVotes.cpp" \
  "$ROOT/drivers/NativeAdreno418/Test/check-rpm-votes.cpp" -o "$OUT/check-rpm-votes"
"$OUT/check-rpm-votes"
"$CXX" -std=c++17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -fsanitize=address,undefined \
  -I "$ROOT/drivers/NativeAdreno418" "$ROOT/drivers/NativeAdreno418/SpmiCore.cpp" \
  "$ROOT/drivers/NativeAdreno418/RpmCore.cpp" "$ROOT/drivers/NativeAdreno418/PinnedGraphics.cpp" \
  "$ROOT/drivers/NativeAdreno418/Test/check-spmi.cpp" -o "$OUT/check-spmi"
"$OUT/check-spmi"
"$CXX" -std=c++17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -fsanitize=address,undefined \
  -I "$ROOT/drivers/NativeAdreno418" "$ROOT/drivers/NativeAdreno418/GpuIo.cpp" \
  "$ROOT/drivers/NativeAdreno418/SecureSmmu.cpp" \
  "$ROOT/drivers/NativeAdreno418/Test/check-secure-smmu.cpp" -o "$OUT/check-secure-smmu"
"$OUT/check-secure-smmu" "$ROOT/drivers/NativeAdreno418/Test/secure-smmu-profile.hex" \
  "$ROOT/drivers/NativeAdreno418/Test/secure-smmu-handset-profile.hex"
