#!/usr/bin/env bash
# Build protoc (third_party/protobuf + third_party/abseil-cpp) with clang into out/protoc.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cmake -S "$ROOT/tools/protobuf" -B "$ROOT/out/protoc" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
ninja -C "$ROOT/out/protoc" protoc
"$ROOT/out/protoc/protobuf/protoc" --version
