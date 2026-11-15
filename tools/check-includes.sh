#!/usr/bin/env bash
# Include-what-you-use check for our C++ (common/, harness/, server_module/):
# every file includes exactly what it uses (no unused includes, no relying on
# transitive includes). Uses clang-include-cleaner.
# Usage: tools/check-includes.sh [--fix]
#   Exit status 1 if anything needs changing (without --fix).
# glibc's internal headers (<bits/...>, <asm-generic/...>) carry no IWYU
# mappings, so they're ignored; include the public header (<pthread.h>,
# <sys/socket.h>) and mark it `// IWYU pragma: keep`.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MODE=--print=changes
[ "${1:-}" = "--fix" ] && MODE=--edit
IGNORE='bits/.*,asm-generic/.*,asm/.*,linux/.*'
FLAGS=(-xc++-header -std=c++20 -Icommon -Ithird_party/googletest/googletest/include)
status=0
check() {  # check <file> [clang-include-cleaner args...]
  local f=$1; shift
  local out
  out=$(clang-include-cleaner "$MODE" --ignore-headers="$IGNORE" "$f" "$@" 2>/dev/null)
  if [ -n "$out" ] && [ "$MODE" = --print=changes ]; then
    echo "== $f"; echo "$out"; status=1
  fi
}
shopt -s nullglob
for f in common/*/*.h; do check "$f" -- "${FLAGS[@]}"; done
for f in common/*/*.cc; do check "$f" -p out/common; done
[ "$MODE" = --edit ] && clang-format -i common/*/*.h common/*/*.cc
[ $status = 0 ] && [ "$MODE" = --print=changes ] && echo "includes OK"
exit $status
