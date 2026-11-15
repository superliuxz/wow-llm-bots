#!/usr/bin/env bash
# Build our shared C++ (common/, linked into mod-llm-brain), then build and
# install AzerothCore into server/.
# Usage: tools/build-server.sh [--test]
#   --test: a test build, with mod-llm-brain's test-only code (src/test/: the
#           fixture reset) compiled in. Without it the server is a production
#           build and contains none of it. Switching only rebuilds the module.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEST_HOOKS=OFF
for arg in "$@"; do
  case "$arg" in
    --test) TEST_HOOKS=ON ;;
    *) echo "unknown option $arg"; exit 2 ;;
  esac
done
"$ROOT/tools/build-common.sh" --server
cmake "$ROOT/out/server" -DLLM_BRAIN_TEST_HOOKS=$TEST_HOOKS > /dev/null
ninja -C "$ROOT/out/server" install
echo "installed a $([ $TEST_HOOKS = ON ] && echo test || echo production) build"
