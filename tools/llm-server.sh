#!/usr/bin/env bash
# Run llama-server (Qwen3.6-35B-A3B) on 127.0.0.1:8090 in the foreground.
# Usage: tools/llm-server.sh [parallel_slots=16] [ctx_per_slot=4096] [extra llama-server args...]
# Defaults are the fastest settings measured for 50-200 bots (docs/phase2-results.md): 16 slots,
# 4k context each, bigger batches (-ub 2048 -b 4096).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NP=${1:-16}; CTX=${2:-4096}; shift $(($# < 2 ? $# : 2))
exec "$ROOT/out/llama/bin/llama-server" \
  -m "$ROOT/models/Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf" \
  --host 127.0.0.1 --port 8090 \
  -ngl 99 -fa on -np "$NP" -c $((NP * CTX)) \
  -ctk q8_0 -ctv q8_0 -ub 2048 -b 4096 --jinja --no-webui "$@"
