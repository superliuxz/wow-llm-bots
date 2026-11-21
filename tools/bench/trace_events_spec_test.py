#!/usr/bin/env python3
"""Checks docs/specs/trace-events.md against the code that writes the traces.

Fails when the spec's sample fields, chat fields, event names, or "Parity" column drift from:
  - the JSON keys in `FormatTraceSample` / `FormatTraceChat` (behavior_trace.cc),
  - every event name the server module emits (`EmitTraceEvent(...)` and `ev.event = ...`),
  - `trace_parity.PARITY_ALLOWED_EVENTS`.
"""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))

import trace_parity  # noqa: E402

SPEC = ROOT / "docs" / "specs" / "trace-events.md"
TRACE_CC = ROOT / "server_module" / "src" / "snapshot" / "behavior_trace.cc"
MODULE_SRC = ROOT / "server_module" / "src"


def _function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def _json_keys(body: str) -> list[str]:
    # Keys appear in the C++ source as \"key\": inside string literals.
    return re.findall(r'\\"([a-z_]+)\\":', body)


def code_sample_fields() -> list[str]:
    return _json_keys(_function_body(TRACE_CC.read_text(), "std::string FormatTraceSample("))


def code_chat_fields() -> list[str]:
    return _json_keys(_function_body(TRACE_CC.read_text(), "std::string FormatTraceChat("))


def code_event_names() -> set[str]:
    names: set[str] = set()
    for path in MODULE_SRC.rglob("*.cc"):
        if path.name.endswith("_unittest.cc"):
            continue
        text = path.read_text()
        # EmitTraceEvent(state, player, "name", ...), possibly split across lines.
        names.update(re.findall(r'EmitTraceEvent\(\s*[^,()]+,\s*[^,()]+,\s*"([a-z_]+)"', text))
        # ev.event = "name";  and  ev.event = cond ? "a" : "b";
        for rhs in re.findall(r'\bev\.event\s*=\s*([^;]+);', text):
            names.update(re.findall(r'"([a-z_]+)"', rhs))
    return names


def _section(text: str, heading: str) -> str:
    start = text.index(heading)
    nxt = text.find("\n## ", start + len(heading))
    return text[start:] if nxt < 0 else text[start:nxt]


def _table_rows(section: str) -> list[list[str]]:
    rows = []
    for line in section.splitlines():
        m = re.match(r"^\|\s*`([a-z_]+)`\s*\|(.*)\|\s*$", line)
        if m:
            rows.append([m.group(1)] + [c.strip() for c in m.group(2).split("|")])
    return rows


class TraceEventsSpecTest(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = SPEC.read_text()

    def test_sample_fields_match_code(self) -> None:
        spec_fields = [r[0] for r in _table_rows(_section(self.spec, '## `kind: "sample"`'))]
        self.assertEqual(spec_fields, code_sample_fields())

    def test_chat_fields_match_code(self) -> None:
        spec_fields = [r[0] for r in _table_rows(_section(self.spec, '## `kind: "chat"`'))]
        self.assertEqual(spec_fields, code_chat_fields())

    def test_event_names_match_code(self) -> None:
        rows = _table_rows(_section(self.spec, '## `kind: "event"`'))
        spec_names = [r[0] for r in rows]
        self.assertEqual(len(spec_names), len(set(spec_names)), "duplicate event rows")
        code = code_event_names()
        self.assertEqual(set(spec_names) - code, set(), "spec lists events the code never emits")
        self.assertEqual(code - set(spec_names), set(), "code emits events missing from the spec")

    def test_parity_column_matches_allow_list(self) -> None:
        rows = _table_rows(_section(self.spec, '## `kind: "event"`'))
        for r in rows:
            self.assertIn(r[-1], ("Yes", "No"), f"bad Parity cell for {r[0]}: {r[-1]!r}")
        spec_yes = {r[0] for r in rows if r[-1] == "Yes"}
        self.assertEqual(spec_yes, set(trace_parity.PARITY_ALLOWED_EVENTS))

    def test_allow_list_only_names_emitted_events(self) -> None:
        self.assertEqual(set(trace_parity.PARITY_ALLOWED_EVENTS) - code_event_names(), set())


if __name__ == "__main__":
    unittest.main()
