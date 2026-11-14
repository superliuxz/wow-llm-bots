#!/usr/bin/env bash
# Build llama.cpp (CUDA, RTX 5090 = sm_120) with clang into out/llama.
# CUDA 13.4 officially supports clang <= 22; we use clang 23 via -allow-unsupported-compiler.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cmake -S "$ROOT/third_party/llama.cpp" -B "$ROOT/out/llama" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DGGML_CUDA=ON -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=clang++ -DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler \
  -DCMAKE_CUDA_ARCHITECTURES=120 \
  -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_CURL=OFF
ninja -C "$ROOT/out/llama" llama-server llama-bench llama-cli
