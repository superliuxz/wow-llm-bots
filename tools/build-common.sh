#!/usr/bin/env bash
# Build the shared C++ code (common/) and run its unit tests.
# Usage: tools/build-common.sh [--no-tests] [--asan | --tsan | --msan | --server]
#   default -> out/common (RelWithDebInfo, hardened std lib)
#   --server -> out/common-server (not hardened: linked into AzerothCore, which
#              isn't hardened; hardening must cover a whole program)
#   --asan  -> out/common-asan (AddressSanitizer + UndefinedBehaviorSanitizer)
#   --tsan  -> out/common-tsan (ThreadSanitizer)
#   --msan  -> out/common-msan (MemorySanitizer, linked against an
#              MSan-instrumented libc++ from tools/setup/build-msan-libcxx.sh; the
#              system libstdc++ isn't instrumented)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TESTS=1; OUT=out/common; SAN=""; TYPE=RelWithDebInfo; HARDENED=ON
for arg in "$@"; do
  case "$arg" in
    --no-tests) TESTS=0 ;;
    --asan) OUT=out/common-asan; SAN="address,undefined"; TYPE=Debug ;;
    --tsan) OUT=out/common-tsan; SAN="thread"; TYPE=Debug ;;
    --msan) OUT=out/common-msan; SAN="memory"; TYPE=Debug ;;
    --server) OUT=out/common-server; HARDENED=OFF; TESTS=0 ;;
    *) echo "unknown option $arg"; exit 2 ;;
  esac
done
B="$ROOT/$OUT"
# Generated proto code needs protoc (same pinned protobuf as the runtime).
[ -x "$ROOT/out/protoc/protobuf/protoc" ] || "$ROOT/tools/build-protoc.sh"
if [ ! -f "$B/build.ninja" ]; then
  FLAGS=""; LDFLAGS="-fuse-ld=lld"
  if [ -n "$SAN" ]; then
    FLAGS="-fsanitize=$SAN -fno-omit-frame-pointer -fno-sanitize-recover=all"
    LDFLAGS="$LDFLAGS -fsanitize=$SAN"
    if [ "$SAN" = memory ]; then
      LIBCXX="$ROOT/out/llvm/msan"
      [ -f "$LIBCXX/lib/libc++.so" ] || "$ROOT/tools/setup/build-msan-libcxx.sh"
      FLAGS="$FLAGS -fsanitize-memory-track-origins -nostdinc++ -isystem $LIBCXX/include/c++/v1"
      LDFLAGS="$LDFLAGS -stdlib=libc++ -L$LIBCXX/lib -Wl,-rpath,$LIBCXX/lib -lc++abi"
    fi
  fi
  cmake -S "$ROOT/common" -B "$B" -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=$TYPE -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
    -DWLB_HARDENED=$HARDENED \
    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" > /dev/null
fi
if [ "$TESTS" = 0 ]; then
  ninja -C "$B" wlb_base wlb_net wlb_proto
else
  ninja -C "$B" wlb_base wlb_net wlb_proto wlb_common_unittests llm_brain_unittests harness_unittests
  "$B/wlb_common_unittests" --gtest_brief=1
  "$B/llm_brain_unittests" --gtest_brief=1
  "$B/harness_unittests" --gtest_brief=1
fi
