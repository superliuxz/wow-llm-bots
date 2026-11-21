#!/usr/bin/env python3
"""Antigravity lifecycle hook for independent post-commit subagent code review.

Events handled (configured in `.agents/hooks.json`):
  - SessionStart: initializes `var/review/last_reviewed_sha` to current `HEAD`
    if missing and writes `var/review/head.patch`.
  - PostToolUse (`run_command`): whenever `HEAD` changes after a shell command
    (e.g. `git commit`), dumps `git show HEAD --stat -p` (plus recent log and
    working-tree status) to `var/review/head.patch` so the read-only `research`
    subagent can read the exact commit patch directly from disk.
  - Stop: if `HEAD` advanced past `var/review/last_reviewed_sha` and no
    `invoke_subagent` call for the new commit has been made yet, returns
    `{"decision": "continue", "reason": "..."}` instructing the agent to spawn
    a fresh, zero-context `research` subagent review before finishing.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
REVIEW_DIR = ROOT / "var" / "review"
HEAD_PATCH_FILE = REVIEW_DIR / "head.patch"
LAST_REVIEWED_FILE = REVIEW_DIR / "last_reviewed_sha"
PROMPT_STATE_FILE = REVIEW_DIR / "prompt_state.json"
MAX_PROMPTS_PER_SHA = 2


def _git(*args: str) -> str:
    res = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    return res.stdout.strip()


def _write_head_patch(full_sha: str) -> None:
    REVIEW_DIR.mkdir(parents=True, exist_ok=True)
    short_sha = full_sha[:7]
    header = f"Commit: {full_sha} ({short_sha})\n"
    if HEAD_PATCH_FILE.exists():
        first_line = HEAD_PATCH_FILE.read_text(errors="replace").splitlines()[:1]
        if first_line and first_line[0].strip() == header.strip():
            return
    branch = _git("rev-parse", "--abbrev-ref", "HEAD")
    recent_log = _git("log", "-n", "5", "--oneline")
    status = _git("status", "--short")
    patch = _git("show", full_sha, "--stat", "-p")
    content = (
        f"{header}"
        f"Branch: {branch}\n\n"
        f"=== Recent Commits (git log -n 5 --oneline) ===\n{recent_log}\n\n"
        f"=== Working Tree Status (git status --short) ===\n{status or '(clean)'}\n\n"
        f"=== Full Commit Patch (git show {short_sha} --stat -p) ===\n{patch}\n"
    )
    HEAD_PATCH_FILE.write_text(content, encoding="utf-8")


def _read_last_reviewed() -> str:
    try:
        return LAST_REVIEWED_FILE.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def _write_last_reviewed(full_sha: str) -> None:
    REVIEW_DIR.mkdir(parents=True, exist_ok=True)
    LAST_REVIEWED_FILE.write_text(full_sha + "\n", encoding="utf-8")


def _subagent_invoked_for_commit(transcript_path_str: str, short_sha: str) -> bool:
    """Returns True if `invoke_subagent` was called for `short_sha` (or after the last `git commit`)."""
    if not transcript_path_str:
        return False
    tpath = pathlib.Path(transcript_path_str)
    if not tpath.is_file():
        return False
    try:
        lines = tpath.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return False

    last_commit_step = -1
    last_subagent_step = -1
    subagent_mentions_sha = False

    for idx, raw in enumerate(lines):
        if not raw.strip():
            continue
        try:
            entry = json.loads(raw)
        except json.JSONDecodeError:
            continue
        tool_calls = entry.get("tool_calls") or []
        for tc in tool_calls:
            name = tc.get("name", "")
            args = tc.get("args") or {}
            if name == "run_command":
                cmd = str(args.get("CommandLine", ""))
                if "git commit" in cmd:
                    last_commit_step = idx
            elif name == "invoke_subagent":
                last_subagent_step = idx
                prompt = str(args.get("Prompt", ""))
                if short_sha and (short_sha in prompt or "var/review/head.patch" in prompt):
                    subagent_mentions_sha = True

    if subagent_mentions_sha and last_subagent_step > last_commit_step:
        return True
    if last_commit_step >= 0 and last_subagent_step > last_commit_step:
        return True
    return False


def _bump_prompt_count(full_sha: str) -> int:
    REVIEW_DIR.mkdir(parents=True, exist_ok=True)
    state: dict[str, int] = {}
    if PROMPT_STATE_FILE.is_file():
        try:
            state = json.loads(PROMPT_STATE_FILE.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            state = {}
    count = int(state.get(full_sha, 0)) + 1
    state = {full_sha: count}
    PROMPT_STATE_FILE.write_text(json.dumps(state) + "\n", encoding="utf-8")
    return count


def main() -> int:
    raw_in = sys.stdin.read()
    try:
        payload = json.loads(raw_in) if raw_in.strip() else {}
    except json.JSONDecodeError:
        payload = {}

    event = payload.get("hookEventName", "")
    full_sha = _git("rev-parse", "HEAD")
    if not full_sha:
        print("{}")
        return 0

    _write_head_patch(full_sha)

    if event == "SessionStart":
        if not LAST_REVIEWED_FILE.is_file():
            _write_last_reviewed(full_sha)
        print("{}")
        return 0

    if event == "PostToolUse":
        print("{}")
        return 0

    if event == "Stop":
        term_reason = payload.get("terminationReason", "")
        fully_idle = bool(payload.get("fullyIdle", True))
        if term_reason != "NO_TOOL_CALL" or not fully_idle:
            print("{}")
            return 0

        last_reviewed = _read_last_reviewed()
        if not last_reviewed:
            _write_last_reviewed(full_sha)
            print("{}")
            return 0

        if full_sha == last_reviewed:
            print("{}")
            return 0

        short_sha = full_sha[:7]
        transcript_path = str(payload.get("transcriptPath", ""))
        if _subagent_invoked_for_commit(transcript_path, short_sha):
            _write_last_reviewed(full_sha)
            print("{}")
            return 0

        prompts = _bump_prompt_count(full_sha)
        if prompts > MAX_PROMPTS_PER_SHA:
            _write_last_reviewed(full_sha)
            print("{}")
            return 0

        reason = (
            f"Commit {short_sha} has not been reviewed by an independent subagent yet, "
            f"and `var/review/head.patch` has been written for {short_sha}. "
            f"Invoke `invoke_subagent` (`TypeName: \"research\"`, "
            f"`Role: \"Independent Commit Reviewer\"`) now with the neutral commit-review "
            f"prompt from `.agents/rules/contributing.md` (Rule #7) for commit {short_sha}, "
            f"then end your turn to wait for the subagent's review."
        )
        print(json.dumps({"decision": "continue", "reason": reason}))
        return 0

    print("{}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
