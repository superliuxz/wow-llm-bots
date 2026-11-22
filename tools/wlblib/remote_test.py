#!/usr/bin/env -S python3 -I
"""Unit tests for tools/wlblib/remote.py and remote-worker helpers in tools/wlblib/server.py."""

from __future__ import annotations

import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from wlblib import remote, server  # noqa: E402
from wlblib.common import Failed  # noqa: E402


class RemoteLibTest(unittest.TestCase):
    def test_load_remotes_missing_and_valid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing = pathlib.Path(tmp) / "nope.json"
            self.assertEqual(remote.load_remotes(missing), {})

            cfg = pathlib.Path(tmp) / "remotes.json"
            cfg.write_text(json.dumps({
                "worker1": {"ssh": "wlb@192.0.2.10", "repo": "~/wow-llm-bots/"},
                "box2": {"ssh": "u@192.0.2.11", "repo": "/srv/wlb", "llm_port": 8091},
            }))
            loaded = remote.load_remotes(cfg)
            self.assertEqual(sorted(loaded), ["box2", "worker1"])
            self.assertEqual(loaded["worker1"].repo, "~/wow-llm-bots")
            self.assertEqual(loaded["worker1"].llm_port, 8090)
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

    def test_rsync_excludes_cover_caches_reference_and_submodule_git(self) -> None:
        self.assertIn(".git", remote.RSYNC_EXCLUDES)
        self.assertIn("/reference/", remote.RSYNC_EXCLUDES)
        self.assertIn("/.cache/", remote.RSYNC_EXCLUDES)
        self.assertIn("__pycache__/", remote.RSYNC_EXCLUDES)

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

            (repo / "a.txt").write_text("v2\n")
            s2 = remote.compute_local_stamp(repo)
            self.assertNotEqual(s1, s2)

            (repo / "new_file.py").write_text("x = 1\n")
            s3 = remote.compute_local_stamp(repo)
            self.assertNotEqual(s2, s3)

            # Excluded directories (out/, reference/, .cache/) do NOT change stamp.
            for d in ("out", "reference", ".cache"):
                (repo / d).mkdir()
                (repo / d / "artifact.bin").write_bytes(b"abc")
            self.assertEqual(s3, remote.compute_local_stamp(repo))

    def test_sync_skips_when_remote_stamp_matches(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "compute_local_stamp", return_value="abc123456789:deadbeef"), \
             mock.patch.object(remote, "read_remote_stamp", return_value="abc123456789:deadbeef:full"), \
             mock.patch.object(remote, "_invalidate_remote_stamp") as m_inv, \
             mock.patch.object(remote, "_rsync_to_remote") as m_rsync, \
             mock.patch.object(remote, "run") as m_run:
            stamp = remote._sync_unlocked(spec, force=False, server_build=True)
            self.assertEqual(stamp, "abc123456789:deadbeef:full")
            stamp_c = remote._sync_unlocked(spec, force=False, server_build=False)
            self.assertEqual(stamp_c, "abc123456789:deadbeef:full")
        m_inv.assert_not_called()
        m_rsync.assert_not_called()
        m_run.assert_not_called()

    def test_common_only_stamp_does_not_satisfy_full_build(self) -> None:
        self.assertFalse(remote._stamp_satisfies("abc:123:common", "abc:123", server_build=True))
        self.assertTrue(remote._stamp_satisfies("abc:123:common", "abc:123", server_build=False))
        self.assertTrue(remote._stamp_satisfies("abc:123:full", "abc:123", server_build=True))

    def test_sync_invalidates_stamp_before_rsync_and_uses_bash_pipefail(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        order: list[str] = []
        with mock.patch.object(remote, "compute_local_stamp", return_value="abc123456789:new"), \
             mock.patch.object(remote, "read_remote_stamp", return_value="abc123456789:old:full"), \
             mock.patch.object(remote, "_invalidate_remote_stamp", side_effect=lambda _: order.append("invalidate")), \
             mock.patch.object(remote, "_rsync_to_remote", side_effect=lambda _: order.append("rsync")), \
             mock.patch.object(remote, "_ensure_remote_map_data", side_effect=lambda _: order.append("maps")), \
             mock.patch.object(remote, "run", side_effect=lambda cmd: order.append(f"build:{cmd[-1]}")) as m_run:
            stamp = remote._sync_unlocked(spec, force=False, server_build=True)
        self.assertEqual(stamp, "abc123456789:new:full")
        self.assertEqual(order[:3], ["invalidate", "rsync", "maps"])
        cmd = m_run.call_args[0][0]
        self.assertTrue(cmd[-1].startswith("bash -euo pipefail -c "))
        self.assertIn("mod-llm-brain", cmd[-1])
        self.assertIn("tools/wlb build common --no-tests", cmd[-1])
        self.assertIn("tools/wlb build server --test", cmd[-1])
        self.assertIn("var/.built_stamp", cmd[-1])

    def test_ensure_remote_map_data_uses_synced_sentinel_and_ssh_opts(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with tempfile.TemporaryDirectory() as tmp:
            repo = pathlib.Path(tmp)
            (repo / "out" / "server" / "data").mkdir(parents=True)
            # Sentinel missing -> rsync runs with -e SSH opts and touches .synced after.
            with mock.patch("subprocess.run", return_value=mock.Mock(returncode=1)) as m_probe, \
                 mock.patch.object(remote, "run") as m_run:
                remote._ensure_remote_map_data(spec, repo=repo)
            self.assertIn(remote.MAP_DATA_SENTINEL_REL, m_probe.call_args[0][0][-1])
            rsync_cmd = m_run.call_args_list[1][0][0]
            self.assertEqual(rsync_cmd[0], "rsync")
            self.assertIn("-e", rsync_cmd)
            self.assertIn("ConnectTimeout=5", rsync_cmd[rsync_cmd.index("-e") + 1])
            self.assertIn("--", rsync_cmd)
            touch_cmd = m_run.call_args_list[2][0][0][-1]
            self.assertIn(f"touch {remote.MAP_DATA_SENTINEL_REL}", touch_cmd)

    def test_pull_validates_paths_and_uses_ssh_opts_and_double_dash(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with self.assertRaises(Failed):
            remote.pull("worker1", "/etc/passwd", "/tmp/x")
        with self.assertRaises(Failed):
            remote.pull("worker1", "../secret", "/tmp/x")
        with tempfile.TemporaryDirectory() as tmp:
            dest = pathlib.Path(tmp) / "sub" / "-weird.json"
            with mock.patch.object(remote, "get_remote", return_value=spec), \
                 mock.patch.object(remote, "run") as m_run:
                remote.pull("worker1", "var/out.json", dest)
            rsync_cmd = m_run.call_args[0][0]
            self.assertIn("-e", rsync_cmd)
            self.assertIn("BatchMode=yes", rsync_cmd[rsync_cmd.index("-e") + 1])
            self.assertEqual(rsync_cmd[-3], "--")
            self.assertEqual(rsync_cmd[-1], str(dest))

    def test_run_remote_rejects_bad_pull_before_running_and_tunnels_setsid(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots", llm_port=8090)
        with mock.patch.object(remote, "get_remote", return_value=spec) as m_get:
            with self.assertRaises(Failed):
                remote.run_remote("worker1", ["echo", "hi"], pulls=["../bad:var/x.json"])
            m_get.assert_not_called()

        fake_proc = mock.Mock(returncode=0)
        with mock.patch.object(remote, "get_remote", return_value=spec), \
             mock.patch.object(remote, "_remote_lock", side_effect=lambda _: mock.MagicMock()), \
             mock.patch.object(remote, "_sync_unlocked", return_value="s") as m_sync, \
             mock.patch.object(remote, "_local_port_open", return_value=True), \
             mock.patch("subprocess.run", return_value=fake_proc) as m_sub:
            remote.run_remote("worker1", ["tools/wlb", "test", "e2e"])
        m_sync.assert_called_once()
        # Normal exit uses a single SSH call (no redundant second SSH rm -f).
        self.assertEqual(m_sub.call_count, 1)
        ssh_cmd = m_sub.call_args[0][0]
        self.assertIn("-R", ssh_cmd)
        self.assertIn("127.0.0.1:8090:127.0.0.1:8090", ssh_cmd)
        self.assertTrue(ssh_cmd[-1].startswith("setsid --wait bash -c "))
        self.assertIn(remote.ACTIVE_PGID_REL, ssh_cmd[-1])
        self.assertIn("trap ", ssh_cmd[-1])

    def test_run_remote_ssh_drop_255_kills_remote_pgid(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "get_remote", return_value=spec), \
             mock.patch.object(remote, "_remote_lock", side_effect=lambda _: mock.MagicMock()), \
             mock.patch.object(remote, "_local_port_open", return_value=False), \
             mock.patch("subprocess.run", return_value=mock.Mock(returncode=255)), \
             mock.patch.object(remote, "_kill_remote_pgid") as m_kill:
            with self.assertRaisesRegex(Failed, r"command failed \(255\)"):
                remote.run_remote("worker1", ["sleep", "60"], no_sync=True)
        m_kill.assert_called_once_with(spec)

    def test_invalidate_rsync_status_and_kill_pgid_commands(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "run") as m_run:
            remote._invalidate_remote_stamp(spec)
        inv_cmd = m_run.call_args[0][0][-1]
        self.assertIn('[ ! -d "$d" ] || rm -f', inv_cmd)
        self.assertNotIn("|| true", inv_cmd)

        with mock.patch.object(remote, "run") as m_run:
            remote._rsync_to_remote(spec, repo=pathlib.Path("/tmp/src"))
        rsync_cmd = m_run.call_args_list[1][0][0]
        self.assertEqual(rsync_cmd[0], "rsync")
        self.assertIn("--delete", rsync_cmd)
        self.assertIn("-e", rsync_cmd)
        self.assertIn("--exclude=.git", rsync_cmd)
        self.assertIn("--", rsync_cmd)

        with mock.patch("subprocess.run") as m_sub:
            remote._kill_remote_pgid(spec)
        kill_cmd = m_sub.call_args[0][0][-1]
        self.assertIn('[ "$PGID" -gt 1 ]', kill_cmd)
        self.assertIn('kill -TERM -"$PGID"', kill_cmd)
        self.assertIn('kill -KILL -"$PGID"', kill_cmd)

        with mock.patch.object(remote, "get_remote", return_value=spec), \
             mock.patch.object(remote, "compute_local_stamp", return_value="abc:123"), \
             mock.patch.object(remote, "read_remote_stamp", return_value="abc:123:full"), \
             mock.patch("subprocess.run", return_value=mock.Mock(stdout="123 ./worldserver\n")) as m_sub:
            remote.status("worker1")
        status_cmd = m_sub.call_args[0][0][-1]
        self.assertIn("/proc/[0-9]*", status_cmd)
        self.assertIn("readlink -f", status_cmd)

    def test_run_remote_interrupt_kills_remote_pgid_before_releasing_lock(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        events: list[str] = []

        @mock.patch.object(remote, "_kill_remote_pgid", side_effect=lambda _: events.append("kill_pgid"))
        def _go(_m_kill):
            class _Lock:
                def __enter__(self):
                    events.append("lock_acquire")
                def __exit__(self, *exc):
                    events.append("lock_release")

            with mock.patch.object(remote, "get_remote", return_value=spec), \
                 mock.patch.object(remote, "_remote_lock", return_value=_Lock()), \
                 mock.patch.object(remote, "_local_port_open", return_value=False), \
                 mock.patch("subprocess.run", side_effect=[KeyboardInterrupt(), mock.Mock(returncode=0)]):
                with self.assertRaises(Failed):
                    remote.run_remote("worker1", ["sleep", "60"], no_sync=True)

        _go()
        self.assertEqual(events, ["lock_acquire", "kill_pgid", "lock_release"])

    def test_run_remote_nonzero_exit_still_attempts_pull_and_raises_cmd_error(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "get_remote", return_value=spec), \
             mock.patch.object(remote, "_remote_lock", side_effect=lambda _: mock.MagicMock()), \
             mock.patch.object(remote, "_local_port_open", return_value=False), \
             mock.patch("subprocess.run", return_value=mock.Mock(returncode=7)), \
             mock.patch.object(remote, "pull", side_effect=Failed("no file")) as m_pull:
            with self.assertRaisesRegex(Failed, r"command failed \(7\)"):
                remote.run_remote("worker1", ["false"], no_sync=True, pulls=["var/a.json:var/b.json"])
        m_pull.assert_called_once()

    def test_parse_remote_argv_strict_flags_and_double_dash(self) -> None:
        # Rejects typos / extra args on non-run actions.
        with self.assertRaises(Failed):
            remote.parse_remote_argv(["list", "extra"])
        with self.assertRaises(Failed):
            remote.parse_remote_argv(["worker1", "sync", "--forse"])
        with self.assertRaises(Failed):
            remote.parse_remote_argv(["worker1", "status", "extra"])
        with self.assertRaises(Failed):
            remote.parse_remote_argv(["worker1", "run", "--unknown", "echo"])

        # Preserves `--force` after `--` for the remote command.
        parsed = remote.parse_remote_argv([
            "worker1", "run", "--pull", "var/a.json:var/b.json", "--no-tunnel",
            "--", "--force", "arg1",
        ])
        self.assertEqual(parsed["pull"], ["var/a.json:var/b.json"])
        self.assertTrue(parsed["no_tunnel"])
        self.assertEqual(parsed["cmd"], ["--force", "arg1"])

        # Stops parsing options at first positional command token.
        parsed2 = remote.parse_remote_argv(["worker1", "run", "python3", "x.py", "--pull", "not_ours"])
        self.assertEqual(parsed2["pull"], [])
        self.assertEqual(parsed2["cmd"], ["python3", "x.py", "--pull", "not_ours"])

        # End-to-end CLI routing through tools/wlb.
        wlb_bin = TOOLS_DIR / "wlb"
        help_res = subprocess.run([str(wlb_bin), "remote", "--help"], capture_output=True, text=True)
        self.assertEqual(help_res.returncode, 0, help_res.stderr)
        self.assertIn("tools/wlb remote list", help_res.stdout)
        bad_res = subprocess.run([str(wlb_bin), "remote", "list", "extra"], capture_output=True, text=True)
        self.assertNotEqual(bad_res.returncode, 0)

    def test_install_hook_worktree_safe_and_refuses_foreign_hook(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            repo = root / "main"
            wt = root / "wt"
            env = {
                **os.environ,
                "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
            }
            subprocess.run(["git", "init", "-q", str(repo)], check=True, env=env)
            (repo / "f.txt").write_text("hi\n")
            subprocess.run(["git", "-C", str(repo), "add", "f.txt"], check=True, env=env)
            subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", "init"], check=True, env=env)
            subprocess.run(["git", "-C", str(repo), "worktree", "add", "-q", "-b", "wt", str(wt)],
                           check=True, env=env)
            self.assertTrue((wt / ".git").is_file())
            with mock.patch.object(remote, "get_remote", return_value=spec):
                hook = remote.install_hook("worker1", repo=wt)
                self.assertTrue(hook.exists())
                body = hook.read_text()
                self.assertIn(remote.HOOK_MARKER, body)
                self.assertIn(str(wt / "var" / "remote"), body)
                remote.install_hook("worker1", repo=wt)
                hook.write_text("#!/bin/sh\necho custom\n")
                with self.assertRaises(Failed):
                    remote.install_hook("worker1", repo=wt)


class ServerRemoteBootstrapTest(unittest.TestCase):
    def test_ensure_local_db_initialized_creates_fresh_atomic_password(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            var = root / "var"
            out = root / "out"
            play_run = out / "server" / "run"
            etc = out / "server" / "etc"
            secrets_dir = var / "secrets"
            db_pw = secrets_dir / "db-password"

            fake_play = mock.Mock(run=play_run)
            with mock.patch.object(server, "VAR", var), \
                 mock.patch.object(server, "LOGS", var / "logs"), \
                 mock.patch.object(server, "PLAY", fake_play), \
                 mock.patch.object(server, "SECRETS", secrets_dir), \
                 mock.patch.object(server, "ETC", etc), \
                 mock.patch.object(server, "DB_PASSWORD", db_pw), \
                 mock.patch.object(server, "MYSQL_CNF", etc / "mysql.cnf"), \
                 mock.patch.object(server, "run") as m_run:
                fresh = server._ensure_local_db_initialized()

            self.assertTrue(fresh)
            self.assertEqual(stat.S_IMODE(play_run.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(secrets_dir.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(db_pw.stat().st_mode), 0o600)
            self.assertEqual(len(db_pw.read_text().strip()), 32)
            m_run.assert_called_once()

    def test_ensure_local_db_initialized_modes_and_legacy_migration(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            var = root / "var"
            out = root / "out"
            play_run = out / "server" / "run"
            etc = out / "server" / "etc"
            secrets_dir = var / "secrets"
            db_pw = secrets_dir / "db-password"
            etc.mkdir(parents=True)
            legacy = etc / ".acore-db-password"
            legacy.write_text("legacy-secret\n")

            fake_play = mock.Mock(run=play_run)
            with mock.patch.object(server, "VAR", var), \
                 mock.patch.object(server, "LOGS", var / "logs"), \
                 mock.patch.object(server, "PLAY", fake_play), \
                 mock.patch.object(server, "SECRETS", secrets_dir), \
                 mock.patch.object(server, "ETC", etc), \
                 mock.patch.object(server, "DB_PASSWORD", db_pw), \
                 mock.patch.object(server, "MYSQL_CNF", etc / "mysql.cnf"), \
                 mock.patch.object(server, "run") as m_run:
                fresh = server._ensure_local_db_initialized()

            self.assertTrue(fresh)
            self.assertEqual(stat.S_IMODE(play_run.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(secrets_dir.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(db_pw.stat().st_mode), 0o600)
            self.assertEqual(db_pw.read_text(), "legacy-secret\n")
            self.assertFalse(legacy.exists())
            m_run.assert_called_once()
            self.assertIn("--initialize-insecure", m_run.call_args[0][0])

    def test_db_start_always_ensures_acore_users_even_when_not_fresh(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "config").mkdir()
            (root / "config" / "mysql.cnf.in").write_text("[mysqld]\n")
            etc = root / "out" / "server" / "etc"
            with mock.patch.object(server, "ROOT", root), \
                 mock.patch.object(server, "ETC", etc), \
                 mock.patch.object(server, "MYSQL_CNF", etc / "mysql.cnf"), \
                 mock.patch.object(server, "_ensure_local_db_initialized", return_value=False), \
                 mock.patch.object(server, "_mysql_up", side_effect=[False, True]), \
                 mock.patch.object(server, "run"), \
                 mock.patch.object(server, "_ensure_acore_users") as m_users:
                server.db_start()
            m_users.assert_called_once()

    def test_golden_key_falls_back_to_var_submodules_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            var = root / "var"
            var.mkdir()
            (var / ".golden_submodules").write_text("aaa111 bbb222\n")
            with mock.patch.object(server, "ROOT", root), \
                 mock.patch.object(server, "VAR", var), \
                 mock.patch.object(server, "output", side_effect=Failed("not a git repo")):
                self.assertEqual(server._golden_key(), "aaa111 bbb222")


if __name__ == "__main__":
    unittest.main()
