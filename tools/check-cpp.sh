#!/usr/bin/env bash
# Everything our C++ must pass before review/commit:
#   1. clang-format (.clang-format)
#   2. include-what-you-use (tools/check-includes.sh)
#   3. build + unit tests: default, ASan+UBSan, TSan, MSan
# Usage: tools/check-cpp.sh
set -euo pipefail
shopt -s globstar nullglob
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
echo "== clang-format"
clang-format --dry-run --Werror common/**/*.h common/**/*.cc harness/**/*.h harness/**/*.cc \
  server_module/**/*.h server_module/**/*.cc
echo "ok"
LOG=$(mktemp); trap 'rm -f "$LOG"' EXIT
for mode in "" --asan --tsan --msan; do
  echo "== build + tests ${mode:-default}"
  # A failed build stops here: never run the test binaries a previous build left behind.
  if ! tools/build-common.sh $mode > "$LOG" 2>&1; then
    grep -E 'error|warning:|FAILED' "$LOG" | head -40
    echo "build failed (${mode:-default})"; exit 1
  fi
  grep -E 'warning:|PASSED|FAILED|Sanitizer|runtime error' "$LOG" || true
  for t in wlb_common_unittests llm_brain_unittests harness_unittests; do
    "out/common${mode:+-${mode#--}}/$t" --gtest_brief=1 > /dev/null
  done
  if [ -z "$mode" ]; then
    # The include check reads the default build's compile commands, so it runs after that build.
    echo "== includes"
    tools/check-includes.sh
    # The screen renderer's goldens come from the Python prototype: it must still write them.
    echo "== screen goldens vs the prototype"
    out/tools-venv/bin/python -I tools/snapshot/make_screen_fixtures.py --check
  fi
done
echo "ALL CHECKS PASSED"
