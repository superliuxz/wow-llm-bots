"""Validate snapshot JSON files against server_module/mod-llm-brain/proto/snapshot.proto.

Each file must parse as a wlb.snapshot.Snapshot in the canonical protobuf JSON mapping
(unknown fields are errors), and must respect visibility:
  - fields marked [(visibility) = HOVER] are only set inside base.tooltips;
  - an open panel (ext field marked OPEN) is only present while base.ui.open_panels
    lists it.

Usage: out/tools-venv/bin/python -I tools/validate_snapshot.py <proto_py_dir> <file.json>...
       (tools/validate-snapshots.sh compiles the proto and checks the examples)
"""

import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
from proto import snapshot_options_pb2 as opts  # noqa: E402  (registers the option extensions)
from proto import snapshot_pb2 as pb  # noqa: E402
from google.protobuf import json_format  # noqa: E402


def field_visibility(fd):
    return fd.GetOptions().Extensions[opts.visibility]


def walk(msg, path, in_tooltip, errors):
    for fd, value in msg.ListFields():
        here = f"{path}.{fd.name}"
        inside = in_tooltip or (msg.DESCRIPTOR.name == "Base" and fd.name == "tooltips")
        if field_visibility(fd) == opts.HOVER and not inside:
            errors.append(f"{here}: hover-only field outside base.tooltips")
        if fd.message_type is None or fd.message_type.GetOptions().map_entry:
            continue
        items = value if fd.is_repeated else [value]
        for i, item in enumerate(items):
            walk(item, f"{here}[{i}]" if fd.is_repeated else here, inside, errors)


def check(snapshot):
    errors = []
    walk(snapshot, "$", False, errors)
    open_panels = {pb.Panel.Name(p) for p in snapshot.base.ui.open_panels}
    for fd, _ in snapshot.ext.ListFields():
        if field_visibility(fd) == opts.OPEN and fd.name.upper() not in open_panels:
            errors.append(f"$.ext.{fd.name}: panel present but not in base.ui.open_panels")
    return errors


failed = 0
for path in sys.argv[2:]:
    try:
        snapshot = json_format.Parse(Path(path).read_text(), pb.Snapshot())
        errors = check(snapshot)
    except json_format.ParseError as e:
        errors = [str(e)]
    if errors:
        failed += 1
        print(f"FAIL {path}")
        for e in errors[:10]:
            print(f"   {e[:200]}")
    else:
        print(f"ok   {path}")
sys.exit(1 if failed else 0)
