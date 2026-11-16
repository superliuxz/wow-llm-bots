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
shopt -s globstar nullglob
for f in common/**/*.h; do check "$f" -- "${FLAGS[@]}"; done
for f in common/**/*.cc; do check "$f" -p out/common; done
# The harness: the same flags, and the build that compiles it.
for f in harness/**/*.h; do check "$f" -- "${FLAGS[@]}" -Iharness/src -Iout/common/gen; done
for f in harness/**/*.cc; do check "$f" -p out/common; done
# Our AzerothCore module: sources use the server build's compile commands;
# its headers only include std and common/ headers.
for f in server_module/**/*.h; do check "$f" -- "${FLAGS[@]}"; done
for f in server_module/**/*.cc; do
  case "$f" in
    *_unittest.cc) check "$f" -p out/common ;;
    */src/test/*)
      # Test-only module code is only in out/server's compile commands for a
      # test build (tools/build-server.sh --test).
      if grep -q "${f#server_module/}" out/server/compile_commands.json; then
        check "$f" -p out/server
      else
        echo "note: $f not checked (out/server is a production build)" >&2
      fi ;;
    *) check "$f" -p out/server ;;
  esac
done
[ "$MODE" = --edit ] && clang-format -i common/**/*.h common/**/*.cc harness/**/*.h harness/**/*.cc server_module/**/*.h server_module/**/*.cc
[ $status = 0 ] && [ "$MODE" = --print=changes ] && echo "includes OK"
exit $status
