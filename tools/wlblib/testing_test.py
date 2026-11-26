#!/usr/bin/env python3
"""Unit tests for parallel e2e: test server shards' names (common.test_instance) and how the
sharded runner splits groups and prints their output (testing.plan_shards, _CaseBlocks)."""

import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from wlblib import common, server, testing  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
