#!/usr/bin/env bash
# Build and install AzerothCore into server/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ninja -C "$ROOT/out/server" install
