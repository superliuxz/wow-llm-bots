#!/usr/bin/env python3
"""Unit tests for dodge_timeline, boss_meter impact-dodge, and bench_bosses._TraceTailer."""

import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))

from tools.bench import bench_bosses, boss_meter, dodge_timeline  # noqa: E402


class DodgeTimelineAndMeterTest(unittest.TestCase):
    def test_dbc_radius_origin_distance_and_dodge_act_filter(self) -> None:
        traces = {
            "BotA": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 5.0,
                },
                {"kind": "event", "t_ms": 1100, "event": "act_sent", "detail": "TARGET Talon King Ikiss"},
                {"kind": "event", "t_ms": 1250, "event": "act_sent", "detail": "AWAY 40"},
                {
                    "kind": "sample", "t_ms": 2000, "in_combat": True,
                    "x": 120.0, "y": 100.0, "moving": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 20.0,
                },
                {
                    "kind": "sample", "t_ms": 3000, "in_combat": True,
                    "x": 140.0, "y": 100.0, "moving": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 40.0,
                },
            ],
            # BotB is healing/not targeting the boss, so target_casting="" and target_dist=-1.
            # Distance must still be measured from the cast origin (100, 100) -> 5.0 yd.
            "BotB": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True,
                    "x": 103.0, "y": 104.0, "moving": False,
                    "target_casting": "", "target_dist": -1.0,
                },
                {
                    "kind": "sample", "t_ms": 3000, "in_combat": True,
                    "x": 103.0, "y": 104.0, "moving": False,
                    "target_casting": "", "target_dist": -1.0,
                },
            ],
        }

        res = dodge_timeline.extract_dodge_timelines(traces, spell_filter="Arcane Explosion")
        self.assertEqual(len(res["windows"]), 1)
        w = res["windows"][0]
        self.assertEqual(w["radius_yd"], 34.0)
        self.assertEqual(w["origin"], [100.0, 100.0])

        a = w["bots"]["BotA"]
        self.assertTrue(a["outside_at_impact"])
        self.assertEqual(a["end_yd"], 40.0)
        self.assertEqual(a["act_delay_ms"], 250)  # Skipped TARGET at 1100, matched AWAY at 1250.
        self.assertEqual(a["act_detail"], "AWAY 40")

        b = w["bots"]["BotB"]
        self.assertEqual(b["end_yd"], 5.0)
        self.assertFalse(b["outside_at_impact"])

    def test_boss_meter_uses_impact_distance_not_max(self) -> None:
        # Bot runs to 40 yd mid-cast, then returns to 20 yd before impact -> NOT outside at impact.
        traces = {
            "BotA": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_dist": 40.0,
                },
                {
                    "kind": "sample", "t_ms": 2000, "in_combat": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_dist": 20.0,
                },
                {
                    "kind": "sample", "t_ms": 2500, "in_combat": True,
                    "target_casting": "", "target_dist": 20.0,
                },
            ],
        }
        res = boss_meter.analyze_boss_fight(traces, boss_name="Talon King Ikiss")
        c = res["characters"]["BotA"]
        self.assertEqual(c["impact_dist_yd"], 20.0)
        self.assertEqual(c["outside_at_impact"], "0/1")

    def test_trace_tailer_incremental_and_partial_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            bdir = root / "bot" / "BotA"
            bdir.mkdir(parents=True)
            f = bdir / "trace.jsonl"
            f.write_text(json.dumps({"t_ms": 100, "kind": "sample"}) + "\n", encoding="utf-8")

            tailer = bench_bosses._TraceTailer(root, ["BotA"])
            rows = tailer.poll()
            self.assertEqual(len(rows["BotA"]), 1)

            # Append an incomplete line without newline: should not be dropped or consumed.
            with f.open("a", encoding="utf-8") as fp:
                fp.write('{"t_ms": 200, "kind": ')
            rows = tailer.poll()
            self.assertEqual(len(rows["BotA"]), 1)

            # Finish the line: should now be parsed on the next poll.
            with f.open("a", encoding="utf-8") as fp:
                fp.write('"event"}\n')
            rows = tailer.poll()
            self.assertEqual(len(rows["BotA"]), 2)
            self.assertEqual(rows["BotA"][1]["t_ms"], 200)

            # Rotate trace.jsonl -> trace.1.jsonl and create a fresh trace.jsonl:
            # rotated file shares inode, so it must NOT be read a second time.
            f.rename(bdir / "trace.1.jsonl")
            f.write_text(json.dumps({"t_ms": 300, "kind": "sample"}) + "\n", encoding="utf-8")
            rows = tailer.poll()
            self.assertEqual(len(rows["BotA"]), 3)
            self.assertEqual([r["t_ms"] for r in rows["BotA"]], [100, 200, 300])

    def test_checkstrat_regex_catches_combat_1_noncombat_0(self) -> None:
        # Substring bug regression: "combat=0" is a substring of "noncombat=0".
        ok, err = bench_bosses._check_strat_line(
            "llmtest: strat tbc-seth on Loada exists=1 combat=1 noncombat=0",
            expected_on=False,
        )
        self.assertFalse(ok)
        self.assertIn("combat=1", err)

        ok, _ = bench_bosses._check_strat_line(
            "llmtest: strat tbc-seth on Loada exists=1 combat=0 noncombat=0",
            expected_on=False,
        )
        self.assertTrue(ok)

        ok, _ = bench_bosses._check_strat_line(
            "llmtest: strat tbc-seth on Loada exists=1 combat=1 noncombat=0",
            expected_on=True,
        )
        self.assertTrue(ok)

    def test_pre_pull_combat_detection(self) -> None:
        traces = {
            "Loada": [
                {"kind": "sample", "t_ms": 1000, "in_combat": False},
                {"kind": "sample", "t_ms": 2500, "in_combat": True},
                {"kind": "sample", "t_ms": 5000, "in_combat": True},
            ],
            "Loadb": [
                {"kind": "sample", "t_ms": 1000, "in_combat": False},
                {"kind": "sample", "t_ms": 5000, "in_combat": True},
            ],
        }
        self.assertTrue(bench_bosses._had_pre_pull_combat(traces, ["Loada"], 1000, 4000))
        self.assertFalse(bench_bosses._had_pre_pull_combat(traces, ["Loadb"], 1000, 4000))

    def test_dodge_timeline_isolates_parallel_instances(self) -> None:
        traces = {
            "Loada": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True, "inst": 1,
                    "x": 140.0, "y": 100.0, "target_casting": "Arcane Explosion",
                    "target_cast_radius": 35.0, "target_x": 100.0, "target_y": 100.0,
                },
            ],
            "Ptwoa": [
                {
                    "kind": "sample", "t_ms": 1000, "in_combat": True, "inst": 2,
                    "x": 105.0, "y": 100.0, "target_casting": "",
                },
            ],
        }
        res = dodge_timeline.extract_dodge_timelines(traces, spell_filter="Arcane Explosion")
        self.assertEqual(len(res["windows"]), 1)
        w = res["windows"][0]
        self.assertEqual(w["instance_id"], 1)
        self.assertIn("Loada", w["bots"])
        self.assertNotIn("Ptwoa", w["bots"])
    def test_wave_arms_fill_every_slot_and_balance_over_a_cycle(self) -> None:
        arms = ("llm", "llm_no_notes", "no_brain", "baseline")
        counts = {a: 0 for a in arms}
        for wave in range(1, 5):  # lcm(4 arms, 3 slots) / 3 slots = 4 waves
            modes = bench_bosses._wave_arms(arms, wave, 3)
            self.assertEqual(len(modes), 3)
            for m in modes:
                counts[m] += 1
        self.assertEqual(set(counts.values()), {3})
        self.assertEqual(bench_bosses._wave_arms(("llm",), 2, 3), ["llm", "llm", "llm"])
        self.assertEqual(bench_bosses._wave_arms(("llm", "llm_no_notes"), 1, 2), ["llm", "llm_no_notes"])
        self.assertEqual(bench_bosses._wave_arms(("llm", "llm_no_notes"), 2, 2), ["llm_no_notes", "llm"])

    def test_console_bot_sends_once_and_waits_for_slow_reply(self) -> None:
        import threading
        import types

        with tempfile.TemporaryDirectory() as tmp:
            logs = pathlib.Path(tmp)
            log = logs / "worldserver.out"
            log.write_text("old line: geared Loada at level 68 (stale)\n", encoding="utf-8")
            calls: list[str] = []

            def fake_console(cmd, wait_s=1.0, inst=None):
                calls.append(cmd)

                def late_reply() -> None:
                    with log.open("a", encoding="utf-8") as fp:
                        fp.write("llmtest: geared Loada at level 68 (role=tank, specNo=3)\n")

                threading.Timer(0.6, late_reply).start()
                return ""  # Reply not there yet when console() returns.

            orig_console, orig_test = bench_bosses.server.console, bench_bosses.TEST
            bench_bosses.server.console = fake_console
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                line, _ = bench_bosses._console_bot(
                    "llmtest gear Loada 68 2", "geared Loada at level 68", wait_s=0.1, timeout_s=3.0
                )
            finally:
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test
            self.assertEqual(calls, ["llmtest gear Loada 68 2"])  # never re-sent
            self.assertIn("specNo=3", line)  # the new reply, not the stale line

    def test_console_bot_token_ignores_late_earlier_where_reply(self) -> None:
        import threading
        import types

        with tempfile.TemporaryDirectory() as tmp:
            logs = pathlib.Path(tmp)
            log = logs / "worldserver.out"
            log.write_text("", encoding="utf-8")
            calls: list[str] = []
            timers: list[threading.Timer] = []

            def fake_console(cmd, wait_s=1.0, inst=None):
                calls.append(cmd)

                def late_old_where() -> None:
                    with log.open("a", encoding="utf-8") as fp:
                        fp.write("llmtest: where Loada map=585 instance=1 alive=1 tok=c1\n")
                        fp.write("llmtest: where Loada map=585 instance=1 alive=1 tok=c20\n")

                def current_where() -> None:
                    with log.open("a", encoding="utf-8") as fp:
                        fp.write("llmtest: where Loada map=0 instance=0 alive=1 tok=c2\n")

                t1 = threading.Timer(0.05, late_old_where)
                t2 = threading.Timer(0.25, current_where)
                timers.extend([t1, t2])
                t1.start()
                t2.start()
                return ""

            orig_console, orig_test = bench_bosses.server.console, bench_bosses.TEST
            bench_bosses.server.console = fake_console
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                line, _ = bench_bosses._console_bot(
                    "llmtest where Loada c2",
                    "where Loada ",
                    wait_s=0.02,
                    timeout_s=2.0,
                    token="c2",
                )
            finally:
                for t in timers:
                    t.join(timeout=2.0)
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test
            self.assertEqual(calls, ["llmtest where Loada c2"])
            self.assertIn("map=0 ", line)
            self.assertIn("tok=c2", line)

    def test_setup_error_before_wave_loop_marks_boss_failed_and_moves_on(self) -> None:
        import types

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            etc = root / "etc"
            (etc / "modules").mkdir(parents=True)
            (etc / "modules" / "mod-llm-brain.conf").write_text("", encoding="utf-8")
            logs = root / "logs"
            logs.mkdir(parents=True)
            var = root / "var"
            var.mkdir(parents=True)
            stops: list[str] = []

            class FakeServer:
                @staticmethod
                def server_stop(inst=None):
                    stops.append("stop")

                @staticmethod
                def test_databases_reset():
                    pass

                @staticmethod
                def config(name, inst=None):
                    pass

                @staticmethod
                def server_start(inst=None):
                    pass

                @staticmethod
                def wait_online(name, inst=None):
                    pass

                @staticmethod
                def wait_online_count(count, inst=None):
                    raise bench_bosses.Failed("simulated pre-wave setup failure")

                @staticmethod
                def console(cmd, wait_s=1.0, inst=None):
                    return ""

            orig_server = bench_bosses.server
            orig_test = bench_bosses.TEST
            orig_var = bench_bosses.VAR
            bench_bosses.server = FakeServer
            bench_bosses.TEST = types.SimpleNamespace(etc=etc, logs=logs)
            bench_bosses.VAR = var
            try:
                res = bench_bosses.run_boss_waves(
                    era="TBC",
                    tag="test_boss",
                    boss_name="Test Boss",
                    level=70,
                    map_id=556,
                    stage_xyz=(0.0, 0.0, 0.0),
                    spawn_xyz=(10.0, 0.0, 0.0),
                    strip_strat="tbc-seth",
                    mode="mixed_all",
                    target_runs=2,
                    parallel=2,
                )
            finally:
                bench_bosses.server = orig_server
                bench_bosses.TEST = orig_test
                bench_bosses.VAR = orig_var

            self.assertTrue(res["failed"])
            self.assertIn("simulated pre-wave setup failure", res["error"])
            self.assertEqual(res["runs"], 0)
            self.assertIn("llm", res["by_arm"])
            self.assertEqual(stops, ["stop", "stop"])

            # Console check RuntimeError during setup also marks boss failed.
            FakeServer.wait_online_count = staticmethod(lambda count, inst=None: None)

            def console_wrong_role(cmd, wait_s=1.0, inst=None):
                parts = cmd.split()
                if len(parts) >= 5 and parts[1] == "group":
                    return f"llmtest: grouped {parts[2]} (leader) and {parts[3]} tok={parts[-1]}\n"
                if len(parts) >= 6 and parts[1] == "gear":
                    name, lvl, tok = parts[2], parts[3], parts[-1]
                    return f"llmtest: geared {name} at level {lvl} (role=dps, hp=100, specNo=3) tok={tok}\n"
                return ""

            FakeServer.console = staticmethod(console_wrong_role)
            bench_bosses.server = FakeServer
            bench_bosses.TEST = types.SimpleNamespace(etc=etc, logs=logs)
            bench_bosses.VAR = var
            try:
                res_console = bench_bosses.run_boss_waves(
                    era="TBC",
                    tag="test_boss_rt",
                    boss_name="Test Boss",
                    level=70,
                    map_id=556,
                    stage_xyz=(0.0, 0.0, 0.0),
                    spawn_xyz=(10.0, 0.0, 0.0),
                    strip_strat="tbc-seth",
                    mode="mixed_all",
                    target_runs=2,
                    parallel=2,
                )
            finally:
                bench_bosses.server = orig_server
                bench_bosses.TEST = orig_test
                bench_bosses.VAR = orig_var
            self.assertTrue(res_console["failed"])
            self.assertIn("role mismatch", res_console["error"])

            # A programming bug during setup must crash loudly, not become "boss failed".
            def bug_in_setup(count, inst=None):
                raise TypeError("bug in bench code")

            FakeServer.wait_online_count = staticmethod(bug_in_setup)
            bench_bosses.server = FakeServer
            bench_bosses.TEST = types.SimpleNamespace(etc=etc, logs=logs)
            bench_bosses.VAR = var
            try:
                with self.assertRaises(TypeError):
                    bench_bosses.run_boss_waves(
                        era="TBC",
                        tag="test_boss_bug",
                        boss_name="Test Boss",
                        level=70,
                        map_id=556,
                        stage_xyz=(0.0, 0.0, 0.0),
                        spawn_xyz=(10.0, 0.0, 0.0),
                        strip_strat="tbc-seth",
                        mode="mixed_all",
                        target_runs=2,
                        parallel=2,
                    )
            finally:
                bench_bosses.server = orig_server
                bench_bosses.TEST = orig_test
                bench_bosses.VAR = orig_var

    def test_wait_harness_connected_returns_on_log_marker(self) -> None:
        import threading
        import time

        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / "harness.log"
            bench_bosses._wait_harness_connected(log, None, timeout_s=2.0)

            class FakeProc:
                def poll(self):
                    return None

            t0 = time.monotonic()

            def write_marker() -> None:
                log.write_text("harness: starting\nharness: connected to mod-llm-brain\n", encoding="utf-8")

            timer = threading.Timer(0.1, write_marker)
            timer.start()
            try:
                bench_bosses._wait_harness_connected(log, FakeProc(), timeout_s=2.0)
            finally:
                timer.join(timeout=2.0)
            elapsed = time.monotonic() - t0
            self.assertLess(elapsed, 1.0)


if __name__ == "__main__":
    unittest.main()
