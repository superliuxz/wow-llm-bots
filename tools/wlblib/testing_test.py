#!/usr/bin/env python3
"""Unit tests for parallel e2e: test server shards' names (common.test_instance) and how the
sharded runner splits groups and prints their output (testing.plan_shards, _CaseBlocks)."""

import io
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from wlblib import build, common, remote, server, testing  # noqa: E402


class TestInstanceTest(unittest.TestCase):
    def test_shard_zero_is_the_plain_test_server(self):
        base = common.test_instance(0)
        self.assertEqual(base.name, "test")
        self.assertEqual(base.run, common.OUT / "server-test" / "run")
        self.assertEqual(base.etc, common.OUT / "server-test" / "etc")
        self.assertEqual(base.etc, base.install_etc)
        self.assertEqual(base.logs, common.LOGS / "test")
        self.assertEqual(base.world_port, 8086)
        self.assertEqual([base.db(n) for n in ("auth", "characters", "playerbots", "world")],
                         ["acore_test_auth", "acore_test_characters", "acore_test_playerbots",
                          "acore_test_world"])

    def test_shard_k_has_its_own_names_and_shares_build_and_world(self):
        base, shard = common.test_instance(0), common.test_instance(3)
        self.assertEqual(shard.name, "test-3")
        self.assertEqual(shard.run, common.OUT / "server-test" / "run-3")
        self.assertEqual(shard.etc, common.OUT / "server-test" / "etc-3")
        self.assertEqual(shard.install_etc, base.etc)
        self.assertEqual(shard.bin, base.bin)
        self.assertEqual(shard.logs, common.LOGS / "test-3")
        self.assertEqual(shard.world_port, 8089)
        self.assertEqual(shard.db("auth"), "acore_test_3_auth")
        self.assertEqual(shard.db("playerbots"), "acore_test_3_playerbots")
        self.assertEqual(shard.db("world"), base.db("world"))
        self.assertTrue(shard.test_hooks)

    def test_shards_never_collide(self):
        shards = [common.test_instance(k) for k in range(common.MAX_SHARDS + 1)]
        for attr in ("name", "run", "etc", "logs", "world_port", "db_prefix"):
            values = [getattr(s, attr) for s in shards]
            self.assertEqual(len(set(values)), len(values), attr)
        self.assertNotIn(common.PLAY.world_port, [s.world_port for s in shards])
        self.assertNotIn(common.PLAY.db_prefix, [s.db_prefix for s in shards])

    def test_out_of_range(self):
        with self.assertRaises(ValueError):
            common.test_instance(-1)
        with self.assertRaises(ValueError):
            common.test_instance(common.MAX_SHARDS + 1)

    def test_env_picks_the_process_test_server(self):
        code = "from wlblib import common; print(common.TEST.name, common.TEST.world_port)"
        def run(value):
            env = {**os.environ, "WLB_TEST_SHARD": value}
            return subprocess.run([sys.executable, "-I", "-c",
                                   f"import sys; sys.path.insert(0, {str(ROOT / 'tools')!r}); "
                                   + code], env=env, capture_output=True, text=True)
        self.assertEqual(run("").stdout.split(), ["test", "8086"])
        self.assertEqual(run("2").stdout.split(), ["test-2", "8088"])
        bad = run("99")
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("WLB_TEST_SHARD", bad.stderr)

    def test_tokens_point_a_shard_at_its_own_paths(self):
        tokens = server._tokens(common.test_instance(2), "pw")
        self.assertEqual(tokens["@DB@"], "acore_test_2_")
        self.assertEqual(tokens["@WORLD_DB@"], "acore_test_world")
        self.assertEqual(tokens["@WORLD_PORT@"], "8088")
        self.assertTrue(tokens["@RUN@"].endswith("server-test/run-2"))
        self.assertTrue(tokens["@LOGS@"].endswith("logs/test-2"))
        play = server._tokens(common.PLAY, "pw")
        self.assertEqual(play["@WORLD_DB@"], "acore_world")

    def test_own_etc_command_mounts_the_shard_etc(self):
        shard = common.test_instance(1)
        cmd = server._with_own_etc(shard, ["./worldserver", "-c", "../etc/worldserver.conf"])
        self.assertEqual(cmd[:4], ["unshare", "--user", "--map-root-user", "--mount"])
        self.assertEqual(cmd[-5:], [str(shard.etc), str(shard.install_etc), "./worldserver",
                                    "-c", "../etc/worldserver.conf"])


def _modules(spec):
    """Fake e2e case modules: spec is [(name, config, bot, online)]."""
    return [types.SimpleNamespace(NAME=n, TEST_CONFIG=c, BOT=b, ONLINE=o) for n, c, b, o in spec]


class PlanShardsTest(unittest.TestCase):
    def test_groups_by_config_bot_online(self):
        plain, groups = testing._groups(_modules([
            ("a", "choose", "Loada", 1), ("b", "whisper", "Loada", 2),
            ("c", "choose", "Loada", 1), ("d", "whisper", "Loada", 4)]))
        self.assertEqual(plain, [])
        self.assertEqual({k: [m.NAME for m in v] for k, v in groups.items()},
                         {("choose", "Loada", 1): ["a", "c"], ("whisper", "Loada", 2): ["b"],
                          ("whisper", "Loada", 4): ["d"]})

    def test_longest_first_on_least_loaded(self):
        groups = {("g1", "B", 1): ["a"], ("g2", "B", 1): ["b"], ("g3", "B", 1): ["c"],
                  ("g4", "B", 1): ["d"]}
        timings = {"g1/B/1": 100, "g2/B/1": 400, "g3/B/1": 300, "g4/B/1": 200}
        shards, load = testing.plan_shards(groups, 2, timings)
        self.assertEqual(shards, [[("g2", "B", 1), ("g1", "B", 1)],
                                  [("g3", "B", 1), ("g4", "B", 1)]])
        self.assertEqual(load, [500, 500])

    def test_every_group_once_and_no_empty_shards(self):
        groups = {(f"g{i}", "B", 1): ["x"] * (i + 1) for i in range(5)}
        for jobs in range(1, 9):
            shards, load = testing.plan_shards(groups, jobs, {})
            self.assertEqual(len(shards), min(jobs, 5))
            self.assertTrue(all(shards))
            flat = [k for s in shards for k in s]
            self.assertEqual(sorted(flat), sorted(groups))

    def test_untimed_group_guessed_by_case_count(self):
        groups = {("big", "B", 1): ["a", "b", "c", "d"], ("small", "B", 1): ["e"],
                  ("timed", "B", 1): ["f"]}
        shards, _ = testing.plan_shards(groups, 3, {"timed/B/1": 1000})
        self.assertEqual([s[0][0] for s in shards], ["timed", "big", "small"])

    def test_one_job_keeps_every_group_on_one_shard(self):
        groups = {("a", "B", 1): ["x"], ("b", "B", 2): ["y"]}
        shards, _ = testing.plan_shards(groups, 1, {})
        self.assertEqual(len(shards), 1)
        self.assertEqual(len(shards[0]), 2)


class DefaultJobsTest(unittest.TestCase):
    def test_bounded_by_ram_cores_and_cap(self):
        self.assertEqual(testing.default_jobs(mem_available_gb=57, cpus=16),
                         common.MAX_SHARDS)
        self.assertEqual(testing.default_jobs(mem_available_gb=20, cpus=16), 2)
        self.assertEqual(testing.default_jobs(mem_available_gb=200, cpus=6), 3)
        self.assertEqual(testing.default_jobs(mem_available_gb=3, cpus=1), 1)


class CaseBlocksTest(unittest.TestCase):
    def test_each_case_printed_whole_with_its_setup_lines(self):
        printed = []
        lock = threading.Lock()
        a = testing._CaseBlocks("test-1", lock, printed.append)
        b = testing._CaseBlocks("test-2", lock, printed.append)
        a.feed("test-1 worldserver started\n")
        b.feed("test-2 worldserver started\n")
        b.feed("PASS e2e choose (3.0 s)\n")
        a.feed("FAIL e2e boss (9.0 s): no plan\n")
        a.feed("1 of 1 passed\n")
        a.flush()
        self.assertEqual(printed, [
            "  [test-2] test-2 worldserver started\nPASS e2e choose (3.0 s)",
            "  [test-1] test-1 worldserver started\nFAIL e2e boss (9.0 s): no plan",
            "  [test-1] 1 of 1 passed"])


class TimingsTest(unittest.TestCase):
    def test_merge_keeps_groups_not_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "e2e-timings.json"
            with mock.patch.object(testing, "TIMINGS", path):
                testing._save_timings({"a/B/1": 10.0, "b/B/1": 20.0})
                testing._save_timings({"b/B/1": 30.0})
                self.assertEqual(testing._load_timings(), {"a/B/1": 10.0, "b/B/1": 30.0})
                path.write_text("not json")
                self.assertEqual(testing._load_timings(), {})


class _FakeProc:
    """Minimal Popen stand-in for testing `testing.all_` orchestration and cleanup."""

    def __init__(self, rc=0, lines=(), wait_exc=None):
        self.rc = rc
        self.stdout = io.StringIO("".join(lines))
        self._wait_exc = wait_exc
        self.waited = False
        self.signals = []
        self.killed = False

    def poll(self):
        return self.rc if self.waited else None

    def send_signal(self, sig):
        self.signals.append(sig)

    def kill(self):
        self.killed = True
        self.waited = True

    def wait(self, timeout=None):
        if self._wait_exc is not None:
            exc = self._wait_exc
            self._wait_exc = None
            raise exc
        self.waited = True
        return self.rc


class TestAllTest(unittest.TestCase):
    def test_commands_default_and_flag_passthrough(self):
        cmds = dict(testing._all_commands())
        wlb = common.ROOT / "tools" / "wlb"
        self.assertEqual(cmds["e2e"], [wlb, "test", "e2e"])
        self.assertEqual(cmds["eval"], [wlb, "test", "eval"])

        cmds = dict(testing._all_commands(jobs=4, keep_server=True, model="m.gguf",
                                          live=True, samples=5, remote="10700k"))
        self.assertEqual(cmds["e2e"],
                         [wlb, "test", "e2e", "-j", "4", "--keep-server", "--remote", "10700k"])
        self.assertEqual(cmds["eval"],
                         [wlb, "test", "eval", "--model", "m.gguf", "--live", "--samples", "5"])

        cmds_serial = dict(testing._all_commands(jobs=1))
        self.assertEqual(cmds_serial["e2e"], [wlb, "test", "e2e", "-j", "1"])

    def _run_all(self, e2e_proc, eval_proc, **kwargs):
        spawned = []
        procs = iter([e2e_proc, eval_proc])

        def fake_popen(cmd, **popen_kwargs):
            spawned.append((cmd, popen_kwargs))
            p = next(procs)
            if isinstance(p, BaseException):
                raise p
            return p

        with mock.patch.object(build, "common") as m_common, \
             mock.patch.object(build, "server") as m_server, \
             mock.patch.object(server, "test_servers_stop") as m_stop, \
             mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
             mock.patch.object(testing, "say"):
            testing.all_(**kwargs)
            return spawned, m_common, m_server, m_stop

    def test_both_pass_prebuilt_and_cleanup(self):
        e2e_p = _FakeProc(rc=0, lines=["PASS e2e choose\n"])
        eval_p = _FakeProc(rc=0, lines=["59 of 59 passed\n"])
        spawned, m_common, m_server, m_stop = self._run_all(e2e_p, eval_p, jobs=3)
        m_common.assert_called_once_with("default", tests=False)
        m_server.assert_called_once_with(test=True)
        m_stop.assert_called_once_with()
        self.assertEqual(len(spawned), 2)
        self.assertIn("-j", spawned[0][0])
        self.assertIn("3", spawned[0][0])
        for _, kw in spawned:
            self.assertEqual(kw["env"][build.PREBUILT_ENV], "1")
            self.assertEqual(kw["env"]["PYTHONUNBUFFERED"], "1")
        self.assertTrue(e2e_p.waited and eval_p.waited)

    def test_keep_server_skips_stop(self):
        e2e_p = _FakeProc(rc=0)
        eval_p = _FakeProc(rc=0)
        _, _, _, m_stop = self._run_all(e2e_p, eval_p, keep_server=True)
        m_stop.assert_not_called()

    def test_exit_code_propagation(self):
        for e2e_rc, eval_rc, expected_in_msg in [
            (1, 0, ("e2e",)),
            (0, 2, ("eval",)),
            (1, 3, ("e2e", "eval")),
        ]:
            e2e_p, eval_p = _FakeProc(rc=e2e_rc), _FakeProc(rc=eval_rc)
            with self.subTest(e2e_rc=e2e_rc, eval_rc=eval_rc):
                with self.assertRaises(common.Failed) as ctx:
                    self._run_all(e2e_p, eval_p)
                for needle in expected_in_msg:
                    self.assertIn(needle, str(ctx.exception))
                self.assertTrue(e2e_p.waited and eval_p.waited)

    def test_keyboard_interrupt_waits_without_duplicate_sigint_and_stops_servers(self):
        e2e_p = _FakeProc(rc=130, wait_exc=KeyboardInterrupt())
        eval_p = _FakeProc(rc=130)
        with mock.patch.object(build, "common"), \
             mock.patch.object(build, "server"), \
             mock.patch.object(server, "test_servers_stop") as m_stop, \
             mock.patch.object(subprocess, "Popen", side_effect=[e2e_p, eval_p]), \
             mock.patch.object(testing, "say"):
            with self.assertRaises(KeyboardInterrupt):
                testing.all_()
        self.assertEqual(e2e_p.signals, [])
        self.assertEqual(eval_p.signals, [])
        self.assertTrue(e2e_p.waited and eval_p.waited)
        m_stop.assert_called_once_with()

    def test_spawn_error_signals_started_child_and_stops_servers(self):
        e2e_p = _FakeProc(rc=0)
        with mock.patch.object(build, "common"), \
             mock.patch.object(build, "server"), \
             mock.patch.object(server, "test_servers_stop") as m_stop, \
             mock.patch.object(subprocess, "Popen", side_effect=[e2e_p, OSError("boom")]), \
             mock.patch.object(testing, "say"):
            with self.assertRaises(OSError):
                testing.all_()
        self.assertEqual(e2e_p.signals, [signal.SIGINT])
        self.assertTrue(e2e_p.waited)
        m_stop.assert_called_once_with()

    def test_stream_lines_prefixes_output(self):
        out = []
        proc = _FakeProc(lines=["hello\n", "world\r\n"])
        testing._stream_lines(proc, "eval", threading.Lock(), emit=out.append)
        self.assertEqual(out, ["[eval] hello", "[eval] world"])

    def test_llama_server_cleans_up_on_startup_interrupt(self):
        fake_proc = mock.MagicMock()
        fake_proc.poll.return_value = None
        srv = testing.LlamaServer(slots=4, timeout_s=30)
        with tempfile.TemporaryDirectory() as tmp, \
             mock.patch.object(testing, "OUT", pathlib.Path(tmp)), \
             mock.patch.object(srv, "healthy", side_effect=[False, KeyboardInterrupt()]), \
             mock.patch.object(subprocess, "Popen", return_value=fake_proc), \
             mock.patch.object(testing, "say"):
            with self.assertRaises(KeyboardInterrupt):
                srv.__enter__()
        fake_proc.terminate.assert_called_once_with()

    def test_cli_target_all_in_targets(self):
        self.assertIn("all", testing.TARGETS)

    def test_all_remote_passthrough_and_early_validation(self):
        e2e_p = _FakeProc(rc=0)
        eval_p = _FakeProc(rc=0)
        with mock.patch.object(remote, "get_remote") as m_get:
            spawned, _, _, _ = self._run_all(e2e_p, eval_p, jobs=4, remote="10700k")
        m_get.assert_called_once_with("10700k")
        self.assertIn("--remote", spawned[0][0])
        self.assertIn("10700k", spawned[0][0])
        self.assertNotIn("--remote", spawned[1][0])

        # Unknown remote fails before building or spawning anything.
        with mock.patch.object(remote, "get_remote", side_effect=common.Failed("unknown remote")), \
             mock.patch.object(build, "common") as m_common, \
             mock.patch.object(build, "server") as m_server:
            with self.assertRaises(common.Failed):
                testing.all_(remote="nope")
        m_common.assert_not_called()
        m_server.assert_not_called()


class RemoteE2eSplitTest(unittest.TestCase):
    def test_split_groups_balanced_and_single_group_local(self):
        groups = {
            ("g1", "B", 1): ["a"],
            ("g2", "B", 1): ["b"],
            ("g3", "B", 1): ["c"],
            ("g4", "B", 1): ["d"],
        }
        timings = {"g1/B/1": 100, "g2/B/1": 400, "g3/B/1": 300, "g4/B/1": 200}
        local_keys, remote_keys = testing.split_groups_for_remote(groups, timings)
        self.assertEqual(local_keys, [("g2", "B", 1), ("g1", "B", 1)])
        self.assertEqual(remote_keys, [("g3", "B", 1), ("g4", "B", 1)])
        self.assertEqual(sorted(local_keys + remote_keys), sorted(groups))

        # Asymmetric capacity: 6 local shards vs 1 remote shard gives remote 1 of 4 bins.
        lk_asym, rk_asym = testing.split_groups_for_remote(
            groups, timings, local_jobs=6, remote_jobs=1,
        )
        self.assertEqual(len(lk_asym), 3)
        self.assertEqual(len(rk_asym), 1)
        self.assertEqual(rk_asym, [("g1", "B", 1)])
        self.assertEqual(sorted(lk_asym + rk_asym), sorted(groups))

        single = {("only", "B", 1): ["a", "b"]}
        lk, rk = testing.split_groups_for_remote(single, {})
        self.assertEqual(lk, [("only", "B", 1)])
        self.assertEqual(rk, [])

    def test_remote_e2e_commands_layout(self):
        wlb = common.ROOT / "tools" / "wlb"
        cmds = testing._remote_e2e_commands(
            ["a", "b"], ["c", "d"], "10700k", jobs=4, keep_server=True
        )
        self.assertEqual(cmds[0][0], "local")
        self.assertEqual(cmds[0][1], [wlb, "test", "e2e", "-j", "4", "--keep-server", "a", "b"])
        self.assertEqual(cmds[0][2][build.PREBUILT_ENV], "1")
        self.assertEqual(cmds[1][0], "remote:10700k")
        self.assertEqual(
            cmds[1][1],
            [wlb, "remote", "10700k", "run", "--no-sync", "--",
             "env", f"{build.PREBUILT_ENV}=1", "tools/wlb", "test", "e2e", "c", "d"],
        )
        # Remote uses its own parallel -j default (no -j flag forwarded).
        self.assertNotIn("-j", cmds[1][1])
        self.assertNotIn("--keep-server", cmds[1][1])

    def _groups(self):
        _, groups = testing._groups(_modules([
            ("a", "g1", "B", 1),
            ("b", "g2", "B", 1),
            ("c", "g3", "B", 1),
            ("d", "g4", "B", 1),
        ]))
        return groups

    def _run_split(self, local_proc, remote_proc, *, jobs=None, keep_server=False):
        spawned = []
        procs = iter([local_proc, remote_proc])

        def fake_popen(cmd, **popen_kwargs):
            spawned.append((cmd, popen_kwargs))
            p = next(procs)
            if isinstance(p, BaseException):
                raise p
            return p

        timings = {"g1/B/1": 100, "g2/B/1": 400, "g3/B/1": 300, "g4/B/1": 200}
        with mock.patch.object(remote, "get_remote") as m_get, \
             mock.patch.object(remote, "sync") as m_sync, \
             mock.patch.object(build, "common") as m_common, \
             mock.patch.object(build, "server") as m_server, \
             mock.patch.object(server, "test_servers_stop") as m_stop, \
             mock.patch.object(testing, "_load_timings", return_value=timings), \
             mock.patch.object(testing, "default_jobs", return_value=1), \
             mock.patch.object(testing, "_remote_default_jobs", return_value=1), \
             mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
             mock.patch.object(testing, "say"):
            ran = testing._run_e2e_remote(self._groups(), jobs, keep_server, "10700k")
            return ran, spawned, m_get, m_sync, m_common, m_server, m_stop

    def test_sync_and_both_halves_pass(self):
        lp = _FakeProc(rc=0, lines=["PASS e2e b\n", "PASS e2e a\n"])
        rp = _FakeProc(rc=0, lines=["PASS e2e c\n", "PASS e2e d\n"])
        ran, spawned, m_get, m_sync, m_common, m_server, m_stop = self._run_split(lp, rp, jobs=1)
        self.assertTrue(ran)
        m_get.assert_called_once_with("10700k")
        m_common.assert_called_once_with("default", tests=False)
        m_server.assert_called_once_with(test=True)
        m_sync.assert_called_once_with("10700k", server_build=True)
        m_stop.assert_called_once_with()
        self.assertEqual(len(spawned), 2)
        self.assertEqual(spawned[0][0][-2:], ["b", "a"])
        self.assertIn("-j", spawned[0][0])
        self.assertIn("1", spawned[0][0])
        self.assertEqual(spawned[0][1]["env"][build.PREBUILT_ENV], "1")
        self.assertEqual(spawned[1][0][-2:], ["c", "d"])
        self.assertIn("--no-sync", spawned[1][0])
        self.assertTrue(lp.waited and rp.waited)

    def test_single_group_skips_remote_sync_and_returns_false(self):
        _, one_group = testing._groups(_modules([("a", "g1", "B", 1)]))
        with mock.patch.object(remote, "get_remote") as m_get, \
             mock.patch.object(remote, "sync") as m_sync, \
             mock.patch.object(build, "server") as m_server, \
             mock.patch.object(testing, "say"):
            ran = testing._run_e2e_remote(one_group, None, False, "10700k")
        self.assertFalse(ran)
        m_get.assert_called_once_with("10700k")
        m_sync.assert_not_called()
        m_server.assert_not_called()

    def test_exit_code_propagation(self):
        for local_rc, remote_rc, expected_in_msg in [
            (1, 0, ("local",)),
            (0, 2, ("remote:10700k",)),
            (1, 3, ("local", "remote:10700k")),
        ]:
            lp, rp = _FakeProc(rc=local_rc), _FakeProc(rc=remote_rc)
            with self.subTest(local_rc=local_rc, remote_rc=remote_rc):
                with self.assertRaises(common.Failed) as ctx:
                    self._run_split(lp, rp)
                for needle in expected_in_msg:
                    self.assertIn(needle, str(ctx.exception))
                self.assertTrue(lp.waited and rp.waited)

    def test_interrupt_and_spawn_error_cleanup(self):
        lp = _FakeProc(rc=130, wait_exc=KeyboardInterrupt())
        rp = _FakeProc(rc=130)
        with mock.patch.object(remote, "get_remote"), \
             mock.patch.object(remote, "sync"), \
             mock.patch.object(build, "common"), \
             mock.patch.object(build, "server"), \
             mock.patch.object(server, "test_servers_stop") as m_stop, \
             mock.patch.object(testing, "_load_timings", return_value={}), \
             mock.patch.object(testing, "_remote_default_jobs", return_value=1), \
             mock.patch.object(subprocess, "Popen", side_effect=[lp, rp]), \
             mock.patch.object(testing, "say"):
            with self.assertRaises(KeyboardInterrupt):
                testing._run_e2e_remote(self._groups(), None, False, "10700k")
        self.assertEqual(lp.signals, [])
        self.assertEqual(rp.signals, [])
        self.assertTrue(lp.waited and rp.waited)
        m_stop.assert_called_once_with()

        lp2 = _FakeProc(rc=0)
        with mock.patch.object(remote, "get_remote"), \
             mock.patch.object(remote, "sync"), \
             mock.patch.object(build, "common"), \
             mock.patch.object(build, "server"), \
             mock.patch.object(server, "test_servers_stop") as m_stop2, \
             mock.patch.object(testing, "_load_timings", return_value={}), \
             mock.patch.object(testing, "_remote_default_jobs", return_value=1), \
             mock.patch.object(subprocess, "Popen", side_effect=[lp2, OSError("boom")]), \
             mock.patch.object(testing, "say"):
            with self.assertRaises(OSError):
                testing._run_e2e_remote(self._groups(), None, False, "10700k")
        self.assertEqual(lp2.signals, [signal.SIGINT])
        self.assertTrue(lp2.waited)
        m_stop2.assert_called_once_with()

    def test_remote_default_jobs_parses_and_falls_back(self):
        ok = subprocess.CompletedProcess([], 0, stdout="banner\n3\n", stderr="")
        with mock.patch.object(subprocess, "run", return_value=ok):
            self.assertEqual(testing._remote_default_jobs("10700k"), 3)
        bad = subprocess.CompletedProcess([], 1, stdout="", stderr="err")
        with mock.patch.object(subprocess, "run", return_value=bad):
            self.assertEqual(testing._remote_default_jobs("10700k"), 1)
        with mock.patch.object(subprocess, "run", side_effect=OSError("no ssh")):
            self.assertEqual(testing._remote_default_jobs("10700k"), 1)

    def test_unknown_remote_fails_before_build_or_sync(self):
        with mock.patch.object(remote, "get_remote", side_effect=common.Failed("unknown remote")), \
             mock.patch.object(remote, "sync") as m_sync, \
             mock.patch.object(build, "server") as m_server:
            with self.assertRaises(common.Failed):
                testing._run_e2e_remote(self._groups(), None, False, "nope")
        m_sync.assert_not_called()
        m_server.assert_not_called()

    def test_cli_rejects_remote_on_non_e2e_targets(self):
        res = subprocess.run(
            [str(ROOT / "tools" / "wlb"), "test", "unit", "--remote", "10700k"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("--remote", res.stderr)


class UnitTestGrammarErrorTest(unittest.TestCase):
    def _run_py(self, code):
        return build.run_unit_test([sys.executable, "-c", code])

    def test_clean_passing_test_succeeds(self):
        buf_out, buf_err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "stdout", buf_out), mock.patch.object(sys, "stderr", buf_err):
            res = self._run_py("print('ok')")
        self.assertEqual(res.returncode, 0)
        self.assertEqual(buf_out.getvalue(), "ok\n")
        self.assertEqual(buf_err.getvalue(), "")

    def test_grammar_parse_error_on_stderr_fails_passing_test(self):
        buf_out, buf_err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "stdout", buf_out), mock.patch.object(sys, "stderr", buf_err):
            with self.assertRaises(common.Failed) as ctx:
                self._run_py(
                    "import sys; print(\"parse: error parsing grammar: expecting ')'\", file=sys.stderr)"
                )
        self.assertIn("parse: error parsing grammar", str(ctx.exception))
        self.assertIn("parse: error parsing grammar", buf_err.getvalue())

    def test_grammar_parse_error_on_stdout_fails_passing_test(self):
        buf_out, buf_err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "stdout", buf_out), mock.patch.object(sys, "stderr", buf_err):
            with self.assertRaises(common.Failed) as ctx:
                self._run_py("print('parse: error parsing grammar: bad rule')")
        self.assertIn("parse: error parsing grammar", str(ctx.exception))
        self.assertIn("parse: error parsing grammar", buf_out.getvalue())

    def test_nonzero_exit_still_fails(self):
        buf_out, buf_err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "stdout", buf_out), mock.patch.object(sys, "stderr", buf_err):
            with self.assertRaises(common.Failed) as ctx:
                self._run_py("import sys; sys.exit(2)")
        self.assertIn("failed (2)", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
