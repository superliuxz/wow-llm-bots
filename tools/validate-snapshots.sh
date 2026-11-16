#!/usr/bin/env bash
# Compile server_module/mod-llm-brain/proto/snapshot.proto, validate the example snapshots (or
# given files), and check the client-event table (docs/specs/client-events.tsv) against the proto.
# Needs out/protoc (tools/build-protoc.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VENV="$ROOT/out/tools-venv"
PROTOC="$ROOT/out/protoc/protobuf/protoc"
GEN="$ROOT/out/proto"
[ -x "$PROTOC" ] || { echo "no protoc; run tools/build-protoc.sh" >&2; exit 1; }
# The Python runtime must match protoc's version (protoc 36.2 <-> protobuf 7.36.2).
PY_PROTOBUF=7.$("$PROTOC" --version | awk '{print $2}')
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/python" -I -c "import google.protobuf as p, sys; sys.exit(p.__version__ != '$PY_PROTOBUF')" 2>/dev/null \
  || "$VENV/bin/pip" install -q "protobuf==$PY_PROTOBUF"
mkdir -p "$GEN"
"$PROTOC" -I"$ROOT/server_module/mod-llm-brain" -I"$ROOT/third_party/protobuf/src" --python_out="$GEN" \
  "$ROOT/server_module/mod-llm-brain/proto/snapshot.proto" "$ROOT/server_module/mod-llm-brain/proto/snapshot_options.proto"
if [ $# -eq 0 ]; then set -- "$ROOT"/docs/specs/examples/*.json; fi
"$VENV/bin/python" -I "$ROOT/tools/validate_snapshot.py" "$GEN" "$@"
"$VENV/bin/python" -I -B "$ROOT/tools/audit/check_client_events.py" "$GEN"
