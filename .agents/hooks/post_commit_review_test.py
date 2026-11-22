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
            self.assertEqual(cmd[:3], ["/usr/bin/agy", "--mode", "plan"])
            self.assertTrue(cmd[-1].startswith("--print="))
            self.assertEqual(kw["env"].get(post_commit_review.REVIEWER_ENV), "1")
            patch_text = (review_dir / f"patch_{c3[:7]}.patch").read_text(encoding="utf-8")
            self.assertIn("commit two", patch_text)
            self.assertIn("commit three", patch_text)
            self.assertEqual((review_dir / "last_reviewed_sha").read_text(encoding="utf-8").strip(), c3)

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
            self.assertEqual(len(calls), 1)
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


if __name__ == "__main__":
    unittest.main()
