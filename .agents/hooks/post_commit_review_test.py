#!/usr/bin/env python3
"""Unit tests for .agents/hooks/post_commit_review.py."""

import fcntl
import pathlib
import subprocess
import sys
import tempfile
import unittest

HOOKS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HOOKS_DIR))
import post_commit_review  # noqa: E402


def _init_repo(repo: pathlib.Path) -> str:
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
    (repo / "a.txt").write_text("v1\n", encoding="utf-8")
    subprocess.run(["git", "add", "a.txt"], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "initial"], cwd=repo, check=True)
    return post_commit_review._git_stdout("rev-parse", "HEAD", cwd=repo)


def _commit(repo: pathlib.Path, filename: str, content: str, msg: str) -> str:
    (repo / filename).write_text(content, encoding="utf-8")
    subprocess.run(["git", "add", filename], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", msg], cwd=repo, check=True)
    return post_commit_review._git_stdout("rev-parse", "HEAD", cwd=repo)


class PostCommitReviewHookTest(unittest.TestCase):
    def test_minimal_env_strips_authoring_chat_vars_and_sets_guard(self):
        env = post_commit_review._minimal_env(
            {
                "HOME": "/home/tester",
                "PATH": "/usr/bin",
                "ANTIGRAVITY_CONVERSATION_ID": "secret-chat-123",
                "AGY_SESSION_TOKEN": "secret-tok",
            }
        )
        self.assertEqual(env["HOME"], "/home/tester")
        self.assertEqual(env["PATH"], "/usr/bin")
        self.assertEqual(env[post_commit_review.REVIEWER_ENV], "1")
        self.assertNotIn("ANTIGRAVITY_CONVERSATION_ID", env)
        self.assertNotIn("AGY_SESSION_TOKEN", env)

    def test_reviews_descendant_range_and_skips_non_descendant_moves(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = pathlib.Path(tmp)
            review_dir = repo / "var" / "review"
            c1 = _init_repo(repo)
            # First run seeds last_reviewed_sha without spawning.
            calls = []
            self.assertEqual(
                post_commit_review.handle_post_tool_use(
                    root=repo, review_dir=review_dir, agy_bin="/bin/true", popen_fn=lambda *a, **k: calls.append((a, k))
                ),
                "seeded",
            )
            self.assertEqual(calls, [])

            # Advance two commits (e.g. cherry-pick/rebase/pull): patch must include both.
            _commit(repo, "b.txt", "b1\n", "commit two")
            c3 = _commit(repo, "c.txt", "c1\n", "commit three")
            status = post_commit_review.handle_post_tool_use(
                root=repo,
                review_dir=review_dir,
                agy_bin="/usr/bin/agy",
                popen_fn=lambda cmd, **kw: calls.append((cmd, kw)),
                skip_review=False,
            )
            self.assertEqual(status, "spawned")
            self.assertEqual(len(calls), 1)
            cmd, kw = calls[0]
            self.assertEqual(cmd[:6], ["/usr/bin/agy", "--mode", "plan", "--sandbox", "--print-timeout", "600s"])
            self.assertTrue(cmd[-1].startswith("--print="))
            self.assertEqual(kw["env"].get(post_commit_review.REVIEWER_ENV), "1")
            patch_text = (review_dir / f"patch_{c3[:7]}.patch").read_text(encoding="utf-8")
            self.assertIn("commit two", patch_text)
            self.assertIn("commit three", patch_text)
            self.assertEqual((review_dir / "last_reviewed_sha").read_text(encoding="utf-8").strip(), c3)

            # Amended commit (same parent, new SHA) MUST be reviewed.
            (repo / "c.txt").write_text("c2-amended\n", encoding="utf-8")
            subprocess.run(["git", "add", "c.txt"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-q", "--amend", "-m", "commit three amended"], cwd=repo, check=True)
            c3_amended = post_commit_review._git_stdout("rev-parse", "HEAD", cwd=repo)
            status_amend = post_commit_review.handle_post_tool_use(
                root=repo,
                review_dir=review_dir,
                agy_bin="/usr/bin/agy",
                popen_fn=lambda cmd, **kw: calls.append((cmd, kw)),
                skip_review=False,
            )
            self.assertEqual(status_amend, "spawned")
            self.assertEqual(len(calls), 2)
            amend_patch = (review_dir / f"patch_{c3_amended[:7]}.patch").read_text(encoding="utf-8")
            self.assertIn("commit three amended", amend_patch)
            self.assertIn("c2-amended", amend_patch)

            # Non-descendant move (reset back to c1) updates marker without spawning.
            subprocess.run(["git", "reset", "--hard", "-q", c1], cwd=repo, check=True)
            status_reset = post_commit_review.handle_post_tool_use(
                root=repo,
                review_dir=review_dir,
                agy_bin="/usr/bin/agy",
                popen_fn=lambda cmd, **kw: calls.append((cmd, kw)),
                skip_review=False,
            )
            self.assertEqual(status_reset, "non_descendant")
            self.assertEqual(len(calls), 2)
            self.assertEqual((review_dir / "last_reviewed_sha").read_text(encoding="utf-8").strip(), c1)

    def test_failed_spawn_does_not_advance_last_reviewed_and_flock_blocks_concurrent(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = pathlib.Path(tmp)
            review_dir = repo / "var" / "review"
            c1 = _init_repo(repo)
            post_commit_review._write_last_reviewed(c1, review_dir / "last_reviewed_sha")
            _commit(repo, "b.txt", "b1\n", "commit two")

            # Missing agy binary must NOT advance last_reviewed_sha.
            status_missing = post_commit_review.handle_post_tool_use(
                root=repo, review_dir=review_dir, agy_bin="", skip_review=False
            )
            self.assertEqual(status_missing, "spawn_failed")
            self.assertEqual((review_dir / "last_reviewed_sha").read_text(encoding="utf-8").strip(), c1)

            # Popen raising OSError must NOT advance last_reviewed_sha.
            def _boom(*args, **kwargs):
                raise OSError("spawn failed")

            status_err = post_commit_review.handle_post_tool_use(
                root=repo, review_dir=review_dir, agy_bin="/usr/bin/agy", popen_fn=_boom, skip_review=False
            )
            self.assertEqual(status_err, "spawn_failed")
            self.assertEqual((review_dir / "last_reviewed_sha").read_text(encoding="utf-8").strip(), c1)

            # Holding review.lock causes concurrent invocation to return 'locked'.
            lock_path = review_dir / "review.lock"
            with lock_path.open("a+", encoding="utf-8") as held:
                fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                try:
                    self.assertEqual(
                        post_commit_review.handle_post_tool_use(
                            root=repo, review_dir=review_dir, agy_bin="/usr/bin/agy", skip_review=False
                        ),
                        "locked",
                    )
                finally:
                    fcntl.flock(held.fileno(), fcntl.LOCK_UN)

    def test_format_review_status_idle_running_and_done(self):
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            review_dir = pathlib.Path(tmp)
            self.assertIn("IDLE", post_commit_review.format_review_status(review_dir))

            out = review_dir / "review_abc1234.md"
            out.write_text("findings\n", encoding="utf-8")
            (review_dir / "latest.md").symlink_to(out.name)
            (review_dir / "active.json").write_text(
                '{"pid": 999999, "sha": "abc1234", "started_at": 100.0, "out": "var/review/review_abc1234.md"}\n',
                encoding="utf-8",
            )
            with mock.patch.object(post_commit_review, "_pid_is_reviewer", return_value=True):
                running = post_commit_review.format_review_status(review_dir)
            self.assertIn("RUNNING", running)
            self.assertIn("abc1234", running)
            self.assertIn("pid=999999", running)

            with mock.patch.object(post_commit_review, "_pid_is_reviewer", return_value=False):
                done = post_commit_review.format_review_status(review_dir)
            self.assertIn("DONE", done)
            self.assertIn("review_abc1234.md", done)

            # 0-byte output with AGY_ERROR/print_timeout in .err reports FAILED instead of DONE.
            out.write_text("", encoding="utf-8")
            (review_dir / "review_abc1234.err").write_text(
                'AGY_ERROR: {"reason":"print_timeout"}\n', encoding="utf-8"
            )
            with mock.patch.object(post_commit_review, "_pid_is_reviewer", return_value=False):
                failed = post_commit_review.format_review_status(review_dir)
            self.assertIn("FAILED (print_timeout)", failed)

            # Non-empty partial output with print_timeout in .err still reports FAILED with exact byte count.
            partial_text = "# Partial truncated review\n"
            out.write_text(partial_text, encoding="utf-8")
            with mock.patch.object(post_commit_review, "_pid_is_reviewer", return_value=False):
                partial_failed = post_commit_review.format_review_status(review_dir)
            self.assertIn("FAILED (print_timeout)", partial_failed)
            self.assertIn(f"({len(partial_text)} bytes)", partial_failed)

            # Capital "Error:" in .err reports FAILED regardless of case.
            out.write_text("", encoding="utf-8")
            (review_dir / "review_abc1234.err").write_text(
                'Error: --print took "--mode"\n', encoding="utf-8"
            )
            with mock.patch.object(post_commit_review, "_pid_is_reviewer", return_value=False):
                cap_err = post_commit_review.format_review_status(review_dir)
            self.assertIn("FAILED (error)", cap_err)

            # Benign info log mentioning "error:" mid-line does NOT falsely mark DONE as FAILED.
            out.write_text("clean review\n", encoding="utf-8")
            (review_dir / "review_abc1234.err").write_text(
                "info: checked handling of error: code paths\n", encoding="utf-8"
            )
            with mock.patch.object(post_commit_review, "_pid_is_reviewer", return_value=False):
                benign_ok = post_commit_review.format_review_status(review_dir)
            self.assertIn("DONE", benign_ok)

            # Empty 0-byte output with clean .err reports FAILED (empty_output).
            out.write_text("", encoding="utf-8")
            (review_dir / "review_abc1234.err").write_text("", encoding="utf-8")
            with mock.patch.object(post_commit_review, "_pid_is_reviewer", return_value=False):
                empty_fail = post_commit_review.format_review_status(review_dir)
            self.assertIn("FAILED (empty_output)", empty_fail)

    def test_watch_review_status_and_cli_flags(self):
        import contextlib
        import io
        from unittest import mock

        # _watch_review_status polls RUNNING -> DONE, prints both lines, and exits cleanly.
        statuses = iter(["RUNNING: independent reviewer for abc1234", "DONE: latest review review_abc1234.md"])
        buf = io.StringIO()
        with mock.patch.object(post_commit_review, "format_review_status", side_effect=lambda _: next(statuses)), \
             mock.patch.object(post_commit_review.time, "sleep") as m_sleep, \
             contextlib.redirect_stdout(buf):
            post_commit_review._watch_review_status(interval_s=0.01)
        m_sleep.assert_called_once_with(0.01)
        self.assertEqual(
            buf.getvalue().splitlines(),
            ["RUNNING: independent reviewer for abc1234", "DONE: latest review review_abc1234.md"],
        )

        # _watch_review_status exits cleanly on KeyboardInterrupt without leaking stdout.
        buf_int = io.StringIO()
        with mock.patch.object(post_commit_review, "format_review_status", return_value="RUNNING: x"), \
             mock.patch.object(post_commit_review.time, "sleep", side_effect=KeyboardInterrupt), \
             contextlib.redirect_stdout(buf_int):
            post_commit_review._watch_review_status(interval_s=0.01)
        self.assertEqual(buf_int.getvalue().splitlines(), ["RUNNING: x"])

        # CLI argument validation on tools/wlb review and post_commit_review.py --status.
        wlb_bin = post_commit_review.ROOT / "tools" / "wlb"
        help_res = subprocess.run([str(wlb_bin), "review", "--help"], capture_output=True, text=True)
        self.assertEqual(help_res.returncode, 0, help_res.stderr)
        self.assertIn("--watch", help_res.stdout)

        bad_wlb = subprocess.run([str(wlb_bin), "review", "--wathc"], capture_output=True, text=True)
        self.assertNotEqual(bad_wlb.returncode, 0)

        hook_bin = HOOKS_DIR / "post_commit_review.py"
        bad_hook = subprocess.run(
            [sys.executable, "-I", "-B", str(hook_bin), "--status", "--bogus"],
            capture_output=True, text=True,
        )
        self.assertEqual(bad_hook.returncode, 2)


if __name__ == "__main__":
    unittest.main()
