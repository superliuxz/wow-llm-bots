#!/usr/bin/env -S python3 -I
"""Unit tests for tools/wlblib/remote.py and remote-worker helpers in tools/wlblib/server.py."""

from __future__ import annotations

import json
import os
import pathlib
import shlex
import shutil
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
                {"ok": {"ssh": "u@h", "repo": "~"}},
                {"ok": {"ssh": "u@h", "repo": "~/"}},
                {"ok": {"ssh": "u@h", "repo": "/"}},
                {"ok": {"ssh": "u@h", "repo": "/."}},
                {"ok": {"ssh": "u@h", "repo": "/home"}},
                {"ok": {"ssh": "u@h", "repo": "/home/."}},
                {"ok": {"ssh": "u@h", "repo": "/tmp/."}},
                {"ok": {"ssh": "u@h", "repo": "~/../other"}},
                {"ok": {"ssh": "u@h", "repo": "~/a;id"}},
                {"ok": {"ssh": "u@h", "repo": "~/my repo"}},
                {"ok": {"ssh": "u@h", "repo": "~/a#b"}},
                {"ok": {"ssh": "u@h", "repo": "$HOME"}},
                {"ok": {"ssh": "u@h", "repo": "/r", "llm_port": True}},
                {"ok": {"ssh": "u@h", "repo": "/r", "llm_port": 70000}},
            ):
                cfg.write_text(json.dumps(bad))
                with self.assertRaises(Failed):
                    remote.load_remotes(cfg)

    def test_verify_remote_sync_target_refuses_home_ancestors_and_missing_sentinel(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            fake_home = root / "home" / "u"
            target = fake_home / "wow-llm-bots"
            target.mkdir(parents=True)
            env = {**os.environ, "HOME": str(fake_home)}

            def _exec_guard(spec: remote.RemoteSpec) -> subprocess.CompletedProcess[str]:
                with mock.patch.object(remote, "run") as m_run:
                    remote._verify_remote_sync_target(spec)
                guard_cmd = m_run.call_args[0][0][-1]
                return subprocess.run(["bash", "-c", guard_cmd], env=env, capture_output=True, text=True)

            # Target without sentinel fails.
            res_no_sentinel = _exec_guard(remote.RemoteSpec(name="w", ssh="u@h", repo=str(target)))
            self.assertNotEqual(res_no_sentinel.returncode, 0)
            self.assertIn("sentinel", res_no_sentinel.stderr)

            # Target equal to $HOME fails even if a sentinel exists.
            (fake_home / remote.REPO_SENTINEL_REL).write_text("")
            res_home = _exec_guard(remote.RemoteSpec(name="w", ssh="u@h", repo=str(fake_home)))
            self.assertNotEqual(res_home.returncode, 0)

            # Ancestor of $HOME fails even if a sentinel exists.
            (root / "home" / remote.REPO_SENTINEL_REL).write_text("")
            res_anc = _exec_guard(remote.RemoteSpec(name="w", ssh="u@h", repo=str(root / "home")))
            self.assertNotEqual(res_anc.returncode, 0)

            # Dedicated subdir with .wlb-remote-repo sentinel succeeds.
            (target / remote.REPO_SENTINEL_REL).write_text("")
            res_ok = _exec_guard(remote.RemoteSpec(name="w", ssh="u@h", repo=str(target)))
            self.assertEqual(res_ok.returncode, 0, res_ok.stderr)

            # Dedicated subdir with tools/wlb sentinel succeeds.
            (target / remote.REPO_SENTINEL_REL).unlink()
            (target / "tools").mkdir()
            (target / "tools" / "wlb").write_text("#!/bin/sh\n")
            res_wlb = _exec_guard(remote.RemoteSpec(name="w", ssh="u@h", repo=str(target)))
            self.assertEqual(res_wlb.returncode, 0, res_wlb.stderr)

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
        self.assertIn("flock -o var/.remote.lock", cmd[-1])
        self.assertIn("var/.built_stamp", cmd[-1])

    def test_rsync_checksum_no_times_preserves_unchanged_and_refreshes_changed_mtime(self) -> None:
        if shutil.which("rsync") is None:
            self.skipTest("rsync not installed")
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "src"
            fake_home = root / "home"
            dst = fake_home / "wow-llm-bots"
            src.mkdir(parents=True)
            dst.mkdir(parents=True)
            (src / "a.cc").write_text("v1\n")
            wrapper = root / "fake_ssh.sh"
            wrapper.write_text(
                f"#!/bin/sh\nexport HOME={shlex.quote(str(fake_home))}\nshift\nexec sh -c \"$*\"\n"
            )
            wrapper.chmod(0o755)
            live_spec = remote.RemoteSpec(name="w", ssh="fakehost", repo="~/wow-llm-bots")
            with mock.patch.object(remote, "_rsync_ssh_cmd", return_value=str(wrapper)), \
                 mock.patch.object(remote, "_verify_remote_sync_target"):
                remote._rsync_to_remote(live_spec, repo=src)
                # Backdate remote copy slightly so we can tell if a second no-op sync touches it.
                old_remote = 1_700_000_000
                os.utime(dst / "a.cc", (old_remote, old_remote))
                remote._rsync_to_remote(live_spec, repo=src)
                self.assertEqual(int((dst / "a.cc").stat().st_mtime), old_remote)

                # Same-length edit with a much older local mtime must still transfer and get a fresh
                # remote mtime so ninja/cmake never keep stale objects.
                (src / "a.cc").write_text("v2\n")
                os.utime(src / "a.cc", (946_684_800, 946_684_800))  # 2000-01-01
                remote._rsync_to_remote(live_spec, repo=src)
                self.assertEqual((dst / "a.cc").read_text(), "v2\n")
                self.assertGreater((dst / "a.cc").stat().st_mtime, old_remote)

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
            self.assertNotIn("-s", rsync_cmd)
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
        self.assertIn("flock -o var/.remote.lock", ssh_cmd[-1])
        self.assertIn("setsid --wait bash -c ", ssh_cmd[-1])
        self.assertIn(remote.ACTIVE_PGID_REL + ".", ssh_cmd[-1])
        self.assertIn("trap ", ssh_cmd[-1])

    def test_run_remote_ssh_drop_255_kills_remote_pgid_only_if_active(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "get_remote", return_value=spec), \
             mock.patch.object(remote, "_remote_lock", side_effect=lambda _: mock.MagicMock()), \
             mock.patch.object(remote, "_local_port_open", return_value=False), \
             mock.patch("subprocess.run", return_value=mock.Mock(returncode=255)), \
             mock.patch.object(remote, "_kill_remote_pgid") as m_kill:
            with self.assertRaisesRegex(Failed, r"command failed \(255\)"):
                remote.run_remote("worker1", ["sleep", "60"], no_sync=True)
        m_kill.assert_called_once()
        self.assertEqual(m_kill.call_args.args, (spec,))
        self.assertTrue(m_kill.call_args.kwargs.get("only_if_active"))
        self.assertTrue(m_kill.call_args.kwargs.get("pgid_rel", "").startswith(remote.ACTIVE_PGID_REL + "."))
        self.assertTrue(m_kill.call_args.kwargs.get("waiter_rel", "").startswith("var/.waiter_remote_pgid."))

    def test_invalidate_rsync_status_and_kill_pgid_commands(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        with mock.patch.object(remote, "run") as m_run:
            remote._invalidate_remote_stamp(spec)
        inv_cmd = m_run.call_args[0][0][-1]
        self.assertIn('[ ! -d "$d" ] ||', inv_cmd)
        self.assertIn("flock -o var/.remote.lock rm -f", inv_cmd)
        self.assertNotIn("|| true", inv_cmd)

        with mock.patch.object(remote, "run") as m_run:
            remote._rsync_to_remote(spec, repo=pathlib.Path("/tmp/src"))
        guard_cmd = m_run.call_args_list[0][0][0][-1]
        self.assertIn(remote.REPO_SENTINEL_REL, guard_cmd)
        rsync_cmd = m_run.call_args_list[1][0][0]
        self.assertEqual(rsync_cmd[0], "rsync")
        self.assertNotIn("-s", rsync_cmd)
        self.assertIn("--delete", rsync_cmd)
        self.assertTrue(any(a.startswith("--rsync-path=") and "flock -o" in a for a in rsync_cmd))
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
        self.assertIn("pwd -P", status_cmd)
        self.assertIn("$PPID", status_cmd)
        self.assertIn("/proc/[0-9]*", status_cmd)

    @unittest.skipUnless(shutil.which("rsync"), "rsync not installed")
    def test_live_rsync_tilde_and_spaced_pull(self) -> None:
        # Live rsync test calling remote.pull() and remote._rsync_to_remote()
        # via a local sh -c SSH wrapper, verifying ~/ expansion and spaced paths.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            fake_home = root / "home"
            src = root / "src"
            dst = fake_home / "wow-llm-bots"
            src.mkdir(parents=True)
            dst.mkdir(parents=True)
            (src / "hello.txt").write_text("ok\n")
            (dst / remote.REPO_SENTINEL_REL).write_text("")
            (dst / "var").mkdir(parents=True)
            (dst / "var" / "my file.json").write_text('{"x":1}\n')
            wrapper = root / "fake_ssh.sh"
            wrapper.write_text(
                f"#!/bin/sh\nexport HOME={shlex.quote(str(fake_home))}\nshift\nexec sh -c \"$*\"\n"
            )
            wrapper.chmod(0o755)
            live_spec = remote.RemoteSpec(name="w", ssh="fakehost", repo="~/wow-llm-bots")
            pulled = root / "pulled.json"
            with mock.patch.object(remote, "_rsync_ssh_cmd", return_value=str(wrapper)), \
                 mock.patch.object(remote, "get_remote", return_value=live_spec), \
                 mock.patch.object(remote, "_verify_remote_sync_target"):
                remote._rsync_to_remote(live_spec, repo=src)
                remote.pull("w", "var/my file.json", pulled)
            self.assertTrue((dst / "hello.txt").is_file())
            self.assertEqual(pulled.read_text(), '{"x":1}\n')
            self.assertFalse((root / "~").exists())

    def test_generated_shell_snippets_execute_correctly_locally(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            real_repo = root / "real_repo"
            sib_repo = root / "real_repo-2"
            link_repo = root / "link_repo"
            (real_repo / "var").mkdir(parents=True)
            sib_repo.mkdir()
            link_repo.symlink_to(real_repo)

            # 1) _invalidate_remote_stamp: missing dir -> ok; existing stamp -> deleted; unwritable -> fails.
            missing_spec = remote.RemoteSpec(name="w", ssh="u@h", repo=str(root / "does_not_exist"))
            with mock.patch.object(remote, "run") as m_run:
                remote._invalidate_remote_stamp(missing_spec)
            res = subprocess.run(["bash", "-c", m_run.call_args[0][0][-1]], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stderr)

            real_spec = remote.RemoteSpec(name="w", ssh="u@h", repo=str(link_repo))
            stamp_file = real_repo / "var" / ".built_stamp"
            stamp_file.write_text("old\n")
            with mock.patch.object(remote, "run") as m_run:
                remote._invalidate_remote_stamp(real_spec)
            inv_cmd = m_run.call_args[0][0][-1]
            res = subprocess.run(["bash", "-c", inv_cmd], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertFalse(stamp_file.exists())

            # 2) _kill_remote_pgid: invalid PGIDs (0, 1, -1, abc) are ignored safely and file removed.
            with mock.patch("subprocess.run") as m_sub:
                remote._kill_remote_pgid(real_spec)
            kill_cmd = m_sub.call_args[0][0][-1]
            pgid_file = real_repo / "var" / ".active_remote_pgid"
            for bad_pgid in ("0", "1", "-1", "abc", ""):
                pgid_file.write_text(bad_pgid + "\n")
                res = subprocess.run(["bash", "-c", kill_cmd], capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertFalse(pgid_file.exists())

            # 2b) _kill_remote_pgid(only_if_active=True): skips test-server stop when pgid file is absent,
            # and kills a real blocked flock waiter via waiter_rel without touching active jobs.
            per_run_rel = f"{remote.ACTIVE_PGID_REL}.deadbeef"
            waiter_rel = "var/.waiter_remote_pgid.deadbeef"
            lock_file = real_repo / "var" / ".remote.lock"
            with lock_file.open("a+") as held_lock:
                import fcntl as _fcntl
                _fcntl.flock(held_lock.fileno(), _fcntl.LOCK_EX | _fcntl.LOCK_NB)
                try:
                    waiter_proc = subprocess.Popen(
                        [
                            "setsid", "--wait", "bash", "-c",
                            f"echo $$ > {real_repo / waiter_rel} && flock -o {lock_file} true",
                        ]
                    )
                    for _ in range(40):
                        if (real_repo / waiter_rel).exists():
                            break
                        import time as _time
                        _time.sleep(0.05)
                    self.assertTrue((real_repo / waiter_rel).exists())
                    with mock.patch("subprocess.run") as m_sub:
                        remote._kill_remote_pgid(
                            real_spec, pgid_rel=per_run_rel, waiter_rel=waiter_rel, only_if_active=True
                        )
                    only_active_cmd = m_sub.call_args[0][0][-1]
                    res_inactive = subprocess.run(["bash", "-c", only_active_cmd], capture_output=True, text=True)
                    self.assertEqual(res_inactive.returncode, 0, res_inactive.stderr)
                    waiter_proc.wait(timeout=5)
                    self.assertFalse((real_repo / waiter_rel).exists())
                finally:
                    _fcntl.flock(held_lock.fileno(), _fcntl.LOCK_UN)

            # 2b-ii) Execute the actual run_remote() setsid+flock+locked_body command locally
            # and verify _kill_remote_pgid kills the active job using the true session PGID.
            captured_cmds: list[str] = []
            with mock.patch.object(remote, "get_remote", return_value=real_spec), \
                 mock.patch.object(remote, "_remote_lock", side_effect=lambda _: mock.MagicMock()), \
                 mock.patch.object(remote, "_local_port_open", return_value=False), \
                 mock.patch("subprocess.run", side_effect=lambda cmd, **kw: (captured_cmds.append(cmd[-1]), mock.Mock(returncode=0))[1]):
                remote.run_remote("w", ["sleep", "30"], no_sync=True)
            active_proc = subprocess.Popen(["bash", "-c", captured_cmds[0]])
            try:
                pgid_files = []
                for _ in range(40):
                    pgid_files = list((real_repo / "var").glob(".active_remote_pgid.*"))
                    if pgid_files:
                        break
                    import time as _time
                    _time.sleep(0.05)
                self.assertEqual(len(pgid_files), 1)
                active_pgid_rel = f"var/{pgid_files[0].name}"
                with mock.patch("subprocess.run") as m_sub:
                    remote._kill_remote_pgid(real_spec, pgid_rel=active_pgid_rel, only_if_active=True)
                kill_active_cmd = m_sub.call_args[0][0][-1]
                subprocess.run(["bash", "-c", kill_active_cmd], capture_output=True, text=True, check=True)
                active_proc.wait(timeout=5)
                self.assertNotEqual(active_proc.returncode, 0)
            finally:
                if active_proc.poll() is None:
                    active_proc.kill()
                    active_proc.wait(timeout=5)

            # 2c) stop(): does not block on REMOTE_LOCK_REL so stuck servers can always be stopped.
            with mock.patch.object(remote, "get_remote", return_value=real_spec), \
                 mock.patch.object(remote, "_remote_lock", side_effect=lambda _: mock.MagicMock()), \
                 mock.patch.object(remote, "run") as m_run:
                remote.stop("w")
            stop_cmd = m_run.call_args[0][0][-1]
            self.assertIn("tools/wlb server stop --test", stop_cmd)
            self.assertNotIn("flock", stop_cmd)

            # 3) status() probe: matches symlinked repo process, ignores sibling-prefix & non-exec $PPID.
            with mock.patch.object(remote, "get_remote", return_value=real_spec), \
                 mock.patch.object(remote, "compute_local_stamp", return_value="a:b"), \
                 mock.patch.object(remote, "read_remote_stamp", return_value="a:b:full"), \
                 mock.patch("subprocess.run", return_value=mock.Mock(stdout="")) as m_sub:
                remote.status("w")
            status_cmd = m_sub.call_args[0][0][-1]

            # Spawn one dummy process in real_repo and one in sibling real_repo-2.
            p_in = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)", "bench_bosses_in.py"],
                cwd=real_repo,
            )
            p_sib = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)", str(sib_repo / "bench_bosses_sib.py")],
                cwd=sib_repo,
            )
            try:
                # Run under a non-exec parent bash (`; true`) to verify $PPID never self-matches.
                res = subprocess.run(
                    ["bash", "-c", f"{status_cmd}; true"],
                    capture_output=True, text=True, check=True,
                )
                lines = [ln.strip() for ln in res.stdout.splitlines() if ln.strip()]
                self.assertTrue(any(ln.startswith(f"{p_in.pid} ") for ln in lines), lines)
                self.assertFalse(any(ln.startswith(f"{p_sib.pid} ") for ln in lines), lines)
                self.assertEqual(len(lines), 1, lines)
            finally:
                p_in.terminate()
                p_sib.terminate()
                p_in.wait(timeout=5)
                p_sib.wait(timeout=5)

    def test_run_remote_interrupt_kills_remote_pgid_before_releasing_lock(self) -> None:
        spec = remote.RemoteSpec(name="worker1", ssh="wlb@192.0.2.10", repo="~/wow-llm-bots")
        events: list[str] = []

        @mock.patch.object(remote, "_kill_remote_pgid", side_effect=lambda *a, **kw: events.append("kill_pgid"))
        def _go(_m_kill):
            class _Lock:
                def __enter__(self):
                    events.append("lock_acquire")
                def __exit__(self, *exc):
                    events.append("lock_release")

            with mock.patch.object(remote, "get_remote", return_value=spec), \
                 mock.patch.object(remote, "_remote_lock", return_value=_Lock()), \
                 mock.patch.object(remote, "_local_port_open", return_value=False), \
                 mock.patch("subprocess.run", side_effect=KeyboardInterrupt()) as m_sub:
                with self.assertRaises(Failed):
                    remote.run_remote("worker1", ["sleep", "60"], no_sync=True)
                self.assertEqual(m_sub.call_count, 1)

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
                # Without allow_init=True, missing datadir must fail (never silently wipe/init play DB).
                with self.assertRaises(Failed):
                    server._ensure_local_db_initialized(allow_init=False)
                fresh = server._ensure_local_db_initialized(allow_init=True)

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
                # Without allow_init=True, missing datadir must fail (never silently wipe/init play DB).
                with self.assertRaises(Failed):
                    server._ensure_local_db_initialized(allow_init=False)
                fresh = server._ensure_local_db_initialized(allow_init=True)

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

    def test_partial_nonempty_datadir_refuses_init_and_drops_tcp_root_users(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            var = root / "var"
            (var / "mysql-data").mkdir(parents=True)
            (var / "mysql-data" / "stray.ibd").write_text("partial\n")
            with mock.patch.object(server, "VAR", var):
                with self.assertRaisesRegex(Failed, r"non-empty"):
                    server._ensure_local_db_initialized(allow_init=True)

        sql_calls: list[str] = []
        with mock.patch.object(server, "_db_password", return_value="pw123"), \
             mock.patch.object(server, "_sql", side_effect=sql_calls.append):
            server._ensure_acore_users()
        self.assertIn("DROP USER IF EXISTS 'root'@'127.0.0.1'", sql_calls)
        self.assertIn("DROP USER IF EXISTS 'root'@'::1'", sql_calls)
        self.assertIn("DROP USER IF EXISTS 'root'@'%'", sql_calls)
        self.assertTrue(any("CREATE USER IF NOT EXISTS 'acore'@'127.0.0.1'" in s for s in sql_calls))

        # CLI `tools/wlb db start --init` parses cleanly.
        wlb_bin = TOOLS_DIR / "wlb"
        res = subprocess.run([str(wlb_bin), "db", "--help"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("--init", res.stdout)


if __name__ == "__main__":
    unittest.main()
