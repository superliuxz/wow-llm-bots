"""The visibility walker's hover-only list matches the tags in snapshot.h.

snapshot/snapshot_wire.cc checks every field by reflection against `kHoverFields`; a field tagged
`// visibility: HOVER` in snapshot/snapshot.h must be in that list, and nothing else may be
(review of f40b0b6, finding 17). Run by `tools/wlb check snapshots`.

Usage: python3 -I -B tools/snapshot/check_hover_fields.py
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent


def tagged(header):
    """"Struct.field" for every `visibility: HOVER` field of `header`'s text."""
    out, struct, previous = [], None, ""
    for line in header.splitlines():
        m = re.match(r"^struct (\w+) \{", line)
        if m:
            struct = m.group(1)
        code = line.split("//")[0]
        if "visibility: HOVER" in line:
            # A declaration clang-format wrapped puts the field name on the line before.
            own = re.search(r"\w+\s*(=[^;]*)?;", code) and not re.match(r"\s*\w+::", code)
            names = re.findall(r"(\w+)\s*(?:=[^;]*)?;", code if own else previous + " " + code)
            if not names:
                raise SystemExit(f"can't read the field on: {line.strip()}")
            out.append(f"{struct}.{names[-1]}")
        previous = code
    return out


def listed(source):
    m = re.search(r"kHoverFields\[\] = \{(.*?)\};", source, re.S)
    if not m:
        raise SystemExit("no kHoverFields in snapshot/snapshot_wire.cc")
    return re.findall(r'"([\w.]+)"', m.group(1))


def main():
    want = tagged((ROOT / "snapshot/snapshot.h").read_text())
    have = listed((ROOT / "snapshot/snapshot_wire.cc").read_text())
    missing = sorted(set(want) - set(have))
    extra = sorted(set(have) - set(want))
    if missing or extra:
        print("kHoverFields (snapshot/snapshot_wire.cc) differs from the visibility: HOVER tags "
              "in snapshot/snapshot.h:", file=sys.stderr)
        for name in missing:
            print(f"  tagged, not listed: {name}", file=sys.stderr)
        for name in extra:
            print(f"  listed, not tagged: {name}", file=sys.stderr)
        return 1
    print(f"hover fields: {len(want)} tagged, all checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
