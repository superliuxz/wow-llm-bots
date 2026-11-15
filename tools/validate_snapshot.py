"""Validate snapshot JSON files against docs/specs/snapshot.schema.json.

Usage: out/tools-venv/bin/python tools/validate_snapshot.py <file.json>...
       (tools/validate-snapshots.sh sets up the venv and checks the examples)
"""

import json
import sys
from pathlib import Path

import jsonschema

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = json.loads((ROOT / "docs/specs/snapshot.schema.json").read_text())
jsonschema.Draft202012Validator.check_schema(SCHEMA)
validator = jsonschema.Draft202012Validator(SCHEMA)

failed = 0
for path in sys.argv[1:]:
    doc = json.loads(Path(path).read_text())
    errors = sorted(validator.iter_errors(doc), key=lambda e: e.path)
    if errors:
        failed += 1
        print(f"FAIL {path}")
        for e in errors[:10]:
            print(f"   {'/'.join(map(str, e.absolute_path)) or '(root)'}: {e.message[:200]}")
    else:
        print(f"ok   {path}")
sys.exit(1 if failed else 0)
