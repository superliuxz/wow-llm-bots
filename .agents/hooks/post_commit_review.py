#!/usr/bin/env python3
"""Antigravity lifecycle hook for non-blocking, separate-conversation post-commit review.

Events handled (configured in `.agents/hooks.json`):
  - SessionStart: initializes `var/review/last_reviewed_sha` to current `HEAD`
    if missing.
  - PostToolUse (`run_command`): whenever `HEAD` advances past
    `var/review/last_reviewed_sha`, writes `var/review/head.patch` (`git show
    HEAD --stat -p` + recent log + status), updates `last_reviewed_sha`, and
    spawns a detached `agy --print --mode plan` process in its own top-level
    conversation (zero shared history, never blocks the authoring chat). Output
    is saved to `var/review/review_<short_sha>.md` (symlinked at
    `var/review/latest.md`).
"""

from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
REVIEW_DIR = ROOT / "var" / "review"
HEAD_PATCH_FILE = REVIEW_DIR / "head.patch"
LAST_REVIEWED_FILE = REVIEW_DIR / "last_reviewed_sha"
LATEST_LINK = REVIEW_DIR / "latest.md"
REVIEWER_ENV = "WLB_COMMIT_REVIEWER"


def _git(*args: str) -> str:
    res = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    return res.stdout.strip()


def _find_agy() -> str | None:
    found = shutil.which("agy")
    if found:
        return found
    candidate = pathlib.Path.home() / ".gemini" / "bin" / "agy"
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    return None


def _write_head_patch(full_sha: str) -> pathlib.Path:
    REVIEW_DIR.mkdir(parents=True, exist_ok=True)
    short_sha = full_sha[:7]
    branch = _git("rev-parse", "--abbrev-ref", "HEAD")
    recent_log = _git("log", "-n", "5", "--oneline")
    status = _git("status", "--short")
    patch = _git("show", full_sha, "--stat", "-p")
    content = (
        f"Commit: {full_sha} ({short_sha})\n"
        f"Branch: {branch}\n\n"
        f"=== Recent Commits (git log -n 5 --oneline) ===\n{recent_log}\n\n"
        f"=== Working Tree Status (git status --short) ===\n{status or '(clean)'}\n\n"
        f"=== Full Commit Patch (git show {short_sha} --stat -p) ===\n{patch}\n"
    )
    patch_file = REVIEW_DIR / f"patch_{short_sha}.patch"
    patch_file.write_text(content, encoding="utf-8")
    HEAD_PATCH_FILE.write_text(content, encoding="utf-8")
    return patch_file


def _read_last_reviewed() -> str:
    try:
        return LAST_REVIEWED_FILE.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def _write_last_reviewed(full_sha: str) -> None:
    REVIEW_DIR.mkdir(parents=True, exist_ok=True)
    LAST_REVIEWED_FILE.write_text(full_sha + "\n", encoding="utf-8")


def _build_review_prompt(short_sha: str, patch_file: pathlib.Path) -> str:
    rel_patch = patch_file.relative_to(ROOT).as_posix()
    return (
        f"Independent cold review of commit {short_sha}: "
        "1) Read CONTRIBUTING.md, docs/TODO.md, and docs/design.md. "
        f"2) Read {rel_patch} and inspect the touched files, headers, tests, and callers in the repo. "
        "3) Audit from two angles: "
        "(a) Code & C++ rules (Chromium C++ style, C++20 in common/snapshot/server_module vs C++26 in harness, "
        "banned std:: facilities vs common/base replacements, 'the brain decides; C++ is a pure harness', "
        "boundary conditions, state interactions, algorithmic complexity claims, and unit/live test coverage); "
        "(b) Honesty & stats/docs (every claim in the commit message, docs/TODO.md, and any report matches the "
        "actual code and var/ artifacts, with multiple-comparison correction where applicable). "
        "Do NOT inspect Antigravity internals or binaries outside the repo. "
        "Report findings grouped by High, Medium, and Low (or 'No findings' if clean)."
    )


def _spawn_reviewer(full_sha: str, patch_file: pathlib.Path) -> None:
    agy_bin = _find_agy()
    if not agy_bin:
        return
    short_sha = full_sha[:7]
    out_file = REVIEW_DIR / f"review_{short_sha}.md"
    err_file = REVIEW_DIR / f"review_{short_sha}.err"
    try:
        if LATEST_LINK.exists() or LATEST_LINK.is_symlink():
            LATEST_LINK.unlink()
        LATEST_LINK.symlink_to(out_file.name)
    except OSError:
        pass

    env = os.environ.copy()
    env[REVIEWER_ENV] = "1"
    prompt = _build_review_prompt(short_sha, patch_file)
    with out_file.open("w", encoding="utf-8") as fout, err_file.open("w", encoding="utf-8") as ferr:
        subprocess.Popen(
            [
                agy_bin,
                "--print",
                "--mode",
                "plan",
                "--print-timeout",
                "300s",
                "--disable-slash-commands",
                prompt,
            ],
            cwd=ROOT,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=fout,
            stderr=ferr,
            start_new_session=True,
        )


def main() -> int:
    # Never trigger recursively inside a spawned reviewer session.
    if os.environ.get(REVIEWER_ENV) == "1":
        print("{}")
        return 0

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

    if event == "SessionStart":
        if not LAST_REVIEWED_FILE.is_file():
            _write_last_reviewed(full_sha)
        print("{}")
        return 0

    if event == "PostToolUse":
        last_reviewed = _read_last_reviewed()
        if not last_reviewed:
            _write_last_reviewed(full_sha)
            print("{}")
            return 0
        if full_sha != last_reviewed:
            patch_file = _write_head_patch(full_sha)
            _write_last_reviewed(full_sha)
            if os.environ.get("WLB_SKIP_COMMIT_REVIEW") != "1":
                _spawn_reviewer(full_sha, patch_file)
        print("{}")
        return 0

    print("{}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
