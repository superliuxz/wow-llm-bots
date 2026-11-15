#!/usr/bin/env bash
# Everything our C++ must pass before review/commit:
#   1. clang-format (Chromium style)
#   2. include-what-you-use (tools/check-includes.sh)
#   3. build + unit tests: default, ASan+UBSan, TSan, MSan
# Usage: tools/check-cpp.sh
set -euo pipefail
shopt -s globstar nullglob
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
echo "== clang-format"
clang-format --dry-run --Werror common/**/*.h common/**/*.cc server_module/*/src/*.h server_module/*/src/*.cc
echo "ok"
echo "== includes"
tools/check-includes.sh
for mode in "" --asan --tsan --msan; do
  echo "== build + tests ${mode:-default}"
  tools/build-common.sh $mode 2>&1 | grep -E 'error|warning:|PASSED|FAILED|Sanitizer|runtime error' || true
  "out/common${mode:+-${mode#--}}/wlb_common_unittests" --gtest_brief=1 > /dev/null
done
echo "ALL CHECKS PASSED"
