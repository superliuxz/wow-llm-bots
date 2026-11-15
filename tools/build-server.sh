#!/usr/bin/env bash
# Build our shared C++ (common/, linked into mod-llm-brain), then build and
# install AzerothCore into server/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
"$ROOT/tools/build-common.sh" --server
ninja -C "$ROOT/out/server" install
