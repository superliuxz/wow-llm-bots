#!/usr/bin/env bash
# Build libc++/libc++abi instrumented with MemorySanitizer into out/llvm/msan,
# matching the installed clang version (MSan needs every byte of C++ code
# instrumented, including the standard library; Chromium does the same).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=$(clang --version | sed -n 's/.*clang version \([0-9.]*\).*/\1/p' | head -1)
SRC="$ROOT/out/llvm/src"
if [ ! -d "$SRC/libcxx" ]; then
  git clone -q --depth 1 --branch "llvmorg-$VER" --filter=blob:none --sparse \
    https://github.com/llvm/llvm-project.git "$SRC"
  git -C "$SRC" sparse-checkout set runtimes libcxx libcxxabi libunwind libc \
    cmake llvm/cmake llvm/utils/llvm-lit third-party
fi
cmake -S "$SRC/runtimes" -B "$ROOT/out/llvm/msan-build" -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" -DLLVM_USE_SANITIZER=MemoryWithOrigins \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DCMAKE_INSTALL_PREFIX="$ROOT/out/llvm/msan"
ninja -C "$ROOT/out/llvm/msan-build" install-cxx install-cxxabi
