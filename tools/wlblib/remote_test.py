#!/usr/bin/env -S python3 -I
"""Unit tests for tools/wlblib/remote.py."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from wlblib import remote  # noqa: E402
from wlblib.common import Failed  # noqa: E402


class RemoteLibTest(unittest.TestCase):
    def test_load_remotes_missing_and_valid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing = pathlib.Path(tmp) / "nope.json"
            self.assertEqual(remote.load_remotes(missing), {})

            cfg = pathlib.Path(tmp) / "remotes.json"
            cfg.write_text(json.dumps({
                "10700k": {"ssh": "wlb@192.0.2.10", "repo": "~/wow-llm-bots/"},
                "box2": {"ssh": "u@192.0.2.11", "repo": "/srv/wlb", "llm_port": 8091},
            }))
            loaded = remote.load_remotes(cfg)
            self.assertEqual(sorted(loaded), ["10700k", "box2"])
            self.assertEqual(loaded["10700k"].repo, "~/wow-llm-bots")
            self.assertEqual(loaded["10700k"].llm_port, 8090)
            self.assertEqual(loaded["box2"].llm_port, 8091)

    def test_load_remotes_rejects_bad_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg = pathlib.Path(tmp) / "remotes.json"
            for bad in (
                {"../escape": {"ssh": "u@h", "repo": "/r"}},
                {"ok": {"ssh": "", "repo": "/r"}},
                {"ok": {"ssh": "u@h", "repo": "/r", "llm_port": True}},
                {"ok": {"ssh": "u@h", "repo": "/r", "llm_port": 70000}},
            ):
                cfg.write_text(json.dumps(bad))
                with self.assertRaises(Failed):
                    remote.load_remotes(cfg)

    def test_remote_cd_expr_handles_tilde_and_spaces(self) -> None:
        self.assertEqual(remote._remote_cd_expr("~"), '"$HOME"')
        self.assertEqual(remote._remote_cd_expr("~/wow-llm-bots"), '"$HOME/"wow-llm-bots')
        self.assertEqual(remote._remote_cd_expr("~/my dir"), '"$HOME/"\'my dir\'')
        self.assertEqual(remote._remote_cd_expr("/srv/my dir"), "'/srv/my dir'")

    def test_compute_local_stamp_changes_on_tracked_and_untracked_edits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = pathlib.Path(tmp)
            env = {
                **os.environ,
                "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
            }
            subprocess.run(["git", "init", "-q", str(repo)], check=True, env=env)
            (repo / "a.txt").write_text("v1\n")
            subprocess.run(["git", "-C", str(repo), "add", "a.txt"], check=True, env=env)
            subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", "init"], check=True, env=env)

            s1 = remote.compute_local_stamp(repo)
            self.assertEqual(s1, remote.compute_local_stamp(repo))

            # Tracked edit changes stamp.
            (repo / "a.txt").write_text("v2\n")
            s2 = remote.compute_local_stamp(repo)
            self.assertNotEqual(s1, s2)

            # Untracked source file changes stamp.
            (repo / "new_file.py").write_text("x = 1\n")
            s3 = remote.compute_local_stamp(repo)
            self.assertNotEqual(s2, s3)

            # Excluded directories (out/, var/) do NOT change stamp.
            (repo / "out").mkdir()
            (repo / "out" / "artifact.bin").write_bytes(b"abc")
            self.assertEqual(s3, remote.compute_local_stamp(repo))

    def test_sync_skips_when_remote_stamp_matches(self) -> None:
        spec = remote.RemoteSpec(name="10700k", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "compute_local_stamp", return_value="abc123456789:deadbeef"), \
             mock.patch.object(remote, "read_remote_stamp", return_value="abc123456789:deadbeef:full"), \
             mock.patch.object(remote, "_rsync_to_remote") as m_rsync, \
             mock.patch.object(remote, "run") as m_run:
            stamp = remote._sync_unlocked(spec, force=False, server_build=True)
            self.assertEqual(stamp, "abc123456789:deadbeef:full")
            # A full stamp also satisfies a common-only sync.
            stamp_c = remote._sync_unlocked(spec, force=False, server_build=False)
            self.assertEqual(stamp_c, "abc123456789:deadbeef:full")
        m_rsync.assert_not_called()
        m_run.assert_not_called()

    def test_common_only_stamp_does_not_satisfy_full_build(self) -> None:
        self.assertFalse(remote._stamp_satisfies("abc:123:common", "abc:123", server_build=True))
        self.assertTrue(remote._stamp_satisfies("abc:123:common", "abc:123", server_build=False))
        self.assertTrue(remote._stamp_satisfies("abc:123:full", "abc:123", server_build=True))

    def test_sync_runs_rsync_and_build_when_stale(self) -> None:
        spec = remote.RemoteSpec(name="10700k", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "compute_local_stamp", return_value="abc123456789:new"), \
             mock.patch.object(remote, "read_remote_stamp", return_value="abc123456789:old:full"), \
             mock.patch.object(remote, "_rsync_to_remote") as m_rsync, \
             mock.patch.object(remote, "_ensure_remote_map_data") as m_maps, \
             mock.patch.object(remote, "run") as m_run:
            stamp = remote._sync_unlocked(spec, force=False, server_build=True)
        self.assertEqual(stamp, "abc123456789:new:full")
        m_rsync.assert_called_once_with(spec)
        m_maps.assert_called_once_with(spec)
        m_run.assert_called_once()
        cmd = m_run.call_args[0][0]
        self.assertIn("mod-llm-brain", cmd[-1])
        self.assertIn("tools/wlb build common --no-tests", cmd[-1])
        self.assertIn("tools/wlb build server --test", cmd[-1])
        self.assertIn("var/.built_stamp", cmd[-1])

    def test_run_remote_adds_reverse_tunnel_when_local_llm_listening(self) -> None:
        spec = remote.RemoteSpec(name="10700k", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots", llm_port=8090)
        fake_proc = mock.Mock(returncode=0)
        with mock.patch.object(remote, "get_remote", return_value=spec), \
             mock.patch.object(remote, "_remote_lock", side_effect=lambda _: mock.MagicMock()), \
             mock.patch.object(remote, "_sync_unlocked", return_value="s") as m_sync, \
             mock.patch.object(remote, "_local_port_open", return_value=True), \
             mock.patch("subprocess.run", return_value=fake_proc) as m_sub:
            remote.run_remote("10700k", ["tools/wlb", "test", "e2e"])
        m_sync.assert_called_once()
        ssh_cmd = m_sub.call_args[0][0]
        self.assertIn("-R", ssh_cmd)
        self.assertIn("127.0.0.1:8090:127.0.0.1:8090", ssh_cmd)
        self.assertIn("exec tools/wlb test e2e", ssh_cmd[-1])

    def test_split_action_flags_parses_flags_after_action(self) -> None:
        opts, cmd = remote.split_action_flags([
            "--pull", "var/a.json:var/b.json",
            "--no-tunnel",
            "--",
            "python3", "tools/bench/bench_bosses.py", "--only", "ikiss",
        ])
        self.assertEqual(opts["pull"], ["var/a.json:var/b.json"])
        self.assertTrue(opts["no_tunnel"])
        self.assertFalse(opts["no_sync"])
        self.assertEqual(cmd, ["python3", "tools/bench/bench_bosses.py", "--only", "ikiss"])

        opts2, cmd2 = remote.split_action_flags(["python3", "x.py", "--pull", "not_ours"])
        self.assertEqual(opts2["pull"], [])
        self.assertEqual(cmd2, ["python3", "x.py", "--pull", "not_ours"])

    def test_install_hook_creates_and_refuses_foreign_hook(self) -> None:
        spec = remote.RemoteSpec(name="10700k", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with tempfile.TemporaryDirectory() as tmp:
            repo = pathlib.Path(tmp)
            (repo / ".git" / "hooks").mkdir(parents=True)
            with mock.patch.object(remote, "get_remote", return_value=spec):
                hook = remote.install_hook("10700k", repo=repo)
                self.assertTrue(hook.exists())
                self.assertIn(remote.HOOK_MARKER, hook.read_text())
                # Re-installing our own hook succeeds.
                remote.install_hook("10700k", repo=repo)
                # Foreign hook is refused.
                hook.write_text("#!/bin/sh\necho custom\n")
                with self.assertRaises(Failed):
                    remote.install_hook("10700k", repo=repo)


if __name__ == "__main__":
    unittest.main()
