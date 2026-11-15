#!/usr/bin/env bash
# Check the snapshot schema and validate the example snapshots (or given files).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VENV="$ROOT/out/tools-venv"
[ -x "$VENV/bin/python" ] || { python3 -m venv "$VENV" && "$VENV/bin/pip" install -q jsonschema; }
if [ $# -eq 0 ]; then set -- "$ROOT"/docs/specs/examples/*.json; fi
"$VENV/bin/python" "$ROOT/tools/validate_snapshot.py" "$@"
