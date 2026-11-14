#!/usr/bin/env bash
# Run llama-server (Qwen3.6-35B-A3B) on 127.0.0.1:8090 in the foreground.
# Usage: tools/llm-server.sh [parallel_slots=25] [ctx_per_slot=4096]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NP=${1:-25}; CTX=${2:-4096}
exec "$ROOT/out/llama/bin/llama-server" \
  -m "$ROOT/models/Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf" \
  --host 127.0.0.1 --port 8090 \
  -ngl 99 -fa on -np "$NP" -c $((NP * CTX)) \
  -ctk q8_0 -ctv q8_0 --jinja --no-webui
