#!/usr/bin/env python3
"""Unit tests for dodge_timeline, boss_meter impact-dodge, and bench_bosses._TraceTailer."""

import json
import os
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

            # Relative trace_dir_override must be resolved to an absolute LlmBrain.TraceDir.
            orig_cwd = os.getcwd()
            os.chdir(tmp)
            try:
                rel_trace = pathlib.Path("rel_trace_dir")
                self.assertFalse(rel_trace.is_absolute())
                bench_bosses.server = FakeServer
                bench_bosses.TEST = types.SimpleNamespace(etc=etc, logs=logs)
                bench_bosses.VAR = var
                try:
                    bench_bosses.run_boss_waves(
                        era="TBC",
                        tag="test_boss_rel",
                        boss_name="Test Boss",
                        level=70,
                        map_id=556,
                        stage_xyz=(0.0, 0.0, 0.0),
                        spawn_xyz=(10.0, 0.0, 0.0),
                        strip_strat="tbc-seth",
                        mode="mixed_all",
                        target_runs=2,
                        parallel=2,
                        trace_dir_override=rel_trace,
                    )
                finally:
                    bench_bosses.server = orig_server
                    bench_bosses.TEST = orig_test
                    bench_bosses.VAR = orig_var
                conf_text = (etc / "modules" / "mod-llm-brain.conf").read_text()
                expected_abs = str((pathlib.Path(tmp) / "rel_trace_dir").resolve())
                self.assertIn(f'LlmBrain.TraceDir = "{expected_abs}"', conf_text)
            finally:
                os.chdir(orig_cwd)

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
            self.assertIn("phase_timings", res_console)
            for key in ("setup_s", "teardown_s", "wall_s", "waves"):
                self.assertIn(key, res_console["phase_timings"])
            self.assertEqual(res_console["phase_timings"]["waves"], [])

            # Verify 2T/3H/5D 10-man raid comp and 360s raid fight caps.
            self.assertEqual(len(bench_bosses.COMP_10), 10)
            roles = [role for _, _, _, role in bench_bosses.COMP_10]
            self.assertEqual(roles.count("tank"), 2)
            self.assertEqual(roles.count("heal"), 3)
            self.assertEqual(roles.count("dps"), 5)
            self.assertEqual(bench_bosses.BOSSES["patchwerk"]["max_s"], 360.0)
            self.assertEqual(bench_bosses.BOSSES["patchwerk"]["clear_trash_yd"], 300.0)
            self.assertEqual(bench_bosses.BOSSES["marrowgar"]["max_s"], 360.0)

            # Verify _clear_trash_for_leaders dispatches cleartrash and raises on failure.
            orig_batch = bench_bosses._console_batch
            seen_specs = []
            try:
                def ok_batch(specs, wait_s=0.0, timeout_s=10.0):
                    seen_specs.extend(specs)
                    return [f"llmtest: cleartrash Loada radius=300 cleared=25 tok={specs[0][2]}"], "ok"

                bench_bosses._console_batch = ok_batch
                bench_bosses._clear_trash_for_leaders(["Loada"], 0.0)
                self.assertEqual(len(seen_specs), 0)

                bench_bosses._clear_trash_for_leaders(["Loada"], 300.0)
                self.assertEqual(len(seen_specs), 1)
                self.assertIn("llmtest cleartrash Loada 300.0 ", seen_specs[0][0])
                self.assertEqual(seen_specs[0][1], "cleartrash Loada ")

                bench_bosses._console_batch = lambda specs, wait_s=0.0, timeout_s=10.0: (
                    [f"llmtest: cleartrash Loada radius=300 cleared=25 tok={specs[0][2]}", ""],
                    "partial err",
                )
                with self.assertRaises(RuntimeError) as ctx:
                    bench_bosses._clear_trash_for_leaders(["Loada", "Ptwoa"], 300.0)
                self.assertIn("cleartrash failed for Ptwoa", str(ctx.exception))
            finally:
                bench_bosses._console_batch = orig_batch

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

    def test_console_batch_serializes_commands_to_avoid_readline_interleaving(self) -> None:
        import threading
        import types

        with tempfile.TemporaryDirectory() as tmp:
            logs = pathlib.Path(tmp)
            log = logs / "worldserver.out"
            log.write_text("", encoding="utf-8")
            calls: list[str] = []
            lock = threading.Lock()
            state = {"in_flight": 0, "max_in_flight": 0}
            timers: list[threading.Timer] = []

            replies = {
                "b1": "AC> llmtest: geared BotA at level 70 (role=tank, specNo=3) tok=b1\r\nAC> ",
                "b2": "llmtest: geared BotB at level 70 (role=heal, specNo=2) tok=b2AC> \r\nAC> ",
                "b3": "AC> llmtest: geared BotC at level 70 (role=dps, specNo=3) tok=b3\r\nAC> ",
            }

            def fake_console(cmd, wait_s=1.0, inst=None):
                calls.append(cmd)
                tok = cmd.split()[-1]
                with lock:
                    state["in_flight"] += 1
                    state["max_in_flight"] = max(state["max_in_flight"], state["in_flight"])

                def finish() -> None:
                    with log.open("a", encoding="utf-8") as fp:
                        fp.write(replies[tok])
                    with lock:
                        state["in_flight"] -= 1

                t = threading.Timer(0.02, finish)
                timers.append(t)
                t.start()
                return ""

            orig_console, orig_test = bench_bosses.server.console, bench_bosses.TEST
            bench_bosses.server.console = fake_console
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                lines, _ = bench_bosses._console_batch(
                    [
                        ("llmtest gear BotA 70 2 b1", "geared BotA at level 70", "b1"),
                        ("llmtest gear BotB 70 1 b2", "geared BotB at level 70", "b2"),
                        ("llmtest gear BotC 70 2 b3", "geared BotC at level 70", "b3"),
                    ],
                    wait_s=0.0,
                    timeout_s=2.0,
                )
            finally:
                for t in timers:
                    t.join(timeout=2.0)
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test

            self.assertEqual(len(calls), 3)
            self.assertEqual(state["max_in_flight"], 1)
            self.assertIn("tok=b1", lines[0])
            self.assertIn("tok=b2", lines[1])
            self.assertIn("tok=b3", lines[2])
            self.assertNotIn("AC> ", lines[1])

            # Early-abort safety: a stalled cmd_1 must prevent cmd_2 from being sent.
            calls.clear()
            bench_bosses.server.console = lambda cmd, wait_s=1.0, inst=None: (calls.append(cmd) or "")
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                stalled, _ = bench_bosses._console_batch(
                    [
                        ("llmtest gear BotA 70 2 stall1", "geared BotA at level 70", "stall1"),
                        ("llmtest gear BotB 70 1 stall2", "geared BotB at level 70", "stall2"),
                    ],
                    wait_s=0.0,
                    timeout_s=0.08,
                )
            finally:
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test
            self.assertEqual(stalled, ["", ""])
            self.assertEqual(calls, ["llmtest gear BotA 70 2 stall1"])

    def test_wait_where_polls_only_pending_bots_until_ready(self) -> None:
        import types

        with tempfile.TemporaryDirectory() as tmp:
            logs = pathlib.Path(tmp)
            queries: list[tuple[str, str]] = []
            pass_num = {"BotA": 0, "BotB": 0}

            def fake_console(cmd, wait_s=1.0, inst=None):
                parts = cmd.split()
                if len(parts) >= 4 and parts[1] == "where":
                    name, tok = parts[2], parts[3]
                    pass_num[name] += 1
                    queries.append((name, tok))
                    if name == "BotA":
                        return f"llmtest: where BotA map=556 instance=7 alive=1 tok={tok}\n"
                    if pass_num["BotB"] == 1:
                        return f"llmtest: where BotB map=556 instance=0 alive=1 tok={tok}\n"
                    return f"llmtest: where BotB map=556 instance=7 alive=1 tok={tok}\n"
                return ""

            orig_console, orig_test = bench_bosses.server.console, bench_bosses.TEST
            bench_bosses.server.console = fake_console
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                res = bench_bosses._wait_where(
                    ["BotA", "BotB"],
                    expected_map=556,
                    require_nonzero_inst=True,
                    timeout_s=3.0,
                )
            finally:
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test

            self.assertEqual([n for n, _ in queries], ["BotA", "BotB", "BotB"])
            self.assertIn("instance=7", res["BotA"])
            self.assertIn("instance=7", res["BotB"])

    def test_slice_traces_after_excludes_prior_wave_kill_across_fast_reset(self) -> None:
        # Fast (~1.0s) inter-wave setup puts Wave 1's mob_kill within 5s of Wave 2 pull_ms;
        # slicing by stage_ms and inst_id must exclude Wave 1 events/samples and map-0 resets.
        rows = [
            {"kind": "sample", "t_ms": 98500, "inst": 1, "in_combat": True},
            {"kind": "event", "t_ms": 98600, "event": "mob_kill", "detail": "Loken", "value": 0},
            {"kind": "sample", "t_ms": 99200, "inst": 0, "in_combat": False},
            {"kind": "sample", "t_ms": 101000, "inst": 5, "in_combat": True},
            {"kind": "event", "t_ms": 102000, "event": "damage_dealt", "detail": "Loken", "value": 5000},
            {"kind": "sample", "t_ms": 105000, "inst": 5, "in_combat": True},
        ]
        sub = bench_bosses._slice_traces_after({"Loada": rows}, ["Loada"], 99500, inst_id=5)
        res = boss_meter.analyze_boss_fight(sub, boss_name="Loken")
        self.assertFalse(res["killed"])
        self.assertEqual(res["duration_s"], 4.0)

    def test_wait_checkstrat_polls_pending_bots_in_single_arm_mode(self) -> None:
        import types

        with tempfile.TemporaryDirectory() as tmp:
            logs = pathlib.Path(tmp)
            queries: list[tuple[str, str]] = []
            pass_num = {"BotA": 0, "BotB": 0}

            def fake_console(cmd, wait_s=1.0, inst=None):
                parts = cmd.split()
                if len(parts) >= 4 and parts[1] == "checkstrat":
                    name, strat, tok = parts[2], parts[3], parts[4]
                    pass_num[name] += 1
                    queries.append((name, tok))
                    # Simulate BotA already stripped, BotB still converging before BotManager::Tick().
                    if name == "BotA":
                        return f"llmtest: strat {strat} on BotA exists=1 combat=0 noncombat=0 tok={tok}\n"
                    if pass_num["BotB"] == 1:
                        return f"llmtest: strat {strat} on BotB exists=1 combat=1 noncombat=0 tok={tok}\n"
                    return f"llmtest: strat {strat} on BotB exists=1 combat=0 noncombat=0 tok={tok}\n"
                return ""

            orig_console, orig_test = bench_bosses.server.console, bench_bosses.TEST
            bench_bosses.server.console = fake_console
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                bench_bosses._wait_checkstrat([("BotA", False), ("BotB", False)], "wotlk-hol", timeout_s=3.0)
            finally:
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test

            self.assertEqual([n for n, _ in queries], ["BotA", "BotB", "BotB"])

            # Also verify expected_on=True (--mode baseline) polling and timeout failure.
            queries.clear()
            pass_num = {"BotA": 0}

            def fake_console_baseline(cmd, wait_s=1.0, inst=None):
                parts = cmd.split()
                if len(parts) >= 4 and parts[1] == "checkstrat":
                    name, strat, tok = parts[2], parts[3], parts[4]
                    pass_num[name] += 1
                    queries.append((name, tok))
                    if pass_num[name] == 1:
                        return f"llmtest: strat {strat} on {name} exists=1 combat=0 noncombat=0 tok={tok}\n"
                    return f"llmtest: strat {strat} on {name} exists=1 combat=1 noncombat=1 tok={tok}\n"
                return ""

            bench_bosses.server.console = fake_console_baseline
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                bench_bosses._wait_checkstrat([("BotA", True)], "wotlk-hol", timeout_s=3.0)
            finally:
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test
            self.assertEqual(len(queries), 2)

            bench_bosses.server.console = (
                lambda cmd, wait_s=1.0, inst=None: (
                    f"llmtest: strat wotlk-hol on BotA exists=1 combat=1 noncombat=1 tok={cmd.split()[-1]}\n"
                    if "checkstrat" in cmd else ""
                )
            )
            bench_bosses.TEST = types.SimpleNamespace(logs=logs)
            try:
                with self.assertRaises(RuntimeError):
                    bench_bosses._wait_checkstrat([("BotA", False)], "wotlk-hol", timeout_s=0.05)
            finally:
                bench_bosses.server.console = orig_console
                bench_bosses.TEST = orig_test

    def test_dodge_dps_lost_and_time_back_in_range(self) -> None:
        # Combat starts at t=0; cast window is [10000, 13000].
        # BotA deals 10000 dmg in [0, 10000) -> dps_before = 1000.0.
        # BotA dodges to 40 yd during cast (0 dmg), returns to 7 yd (<= 5 + 3) at t=15000.
        traces = {
            "BotA": [
                {"kind": "sample", "t_ms": 0, "in_combat": True, "x": 105.0, "y": 100.0, "moving": False},
                {"kind": "event", "t_ms": 2000, "event": "damage_dealt", "value": 5000},
                {"kind": "event", "t_ms": 7000, "event": "damage_dealt", "value": 5000},
                {
                    "kind": "sample", "t_ms": 10000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 5.0,
                },
                {"kind": "event", "t_ms": 10200, "event": "act_sent", "detail": "AWAY 40"},
                {
                    "kind": "sample", "t_ms": 11500, "in_combat": True,
                    "x": 125.0, "y": 100.0, "moving": True,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 25.0,
                },
                {
                    "kind": "sample", "t_ms": 13000, "in_combat": True,
                    "x": 140.0, "y": 100.0, "moving": True,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0, "target_dist": 40.0,
                },
                {"kind": "sample", "t_ms": 14000, "in_combat": True, "x": 120.0, "y": 100.0, "moving": True},
                {"kind": "sample", "t_ms": 15000, "in_combat": True, "x": 107.0, "y": 100.0, "moving": False, "casting": "Lightning Bolt"},
            ],
        }
        res = dodge_timeline.extract_dodge_timelines(traces, spell_filter="Lightning Nova")
        self.assertEqual(len(res["windows"]), 1)
        a = res["windows"][0]["bots"]["BotA"]
        self.assertEqual(a["dps_before"], 1000.0)
        self.assertEqual(a["time_back_in_range_ms"], 2000)
        self.assertEqual(a["downtime_s"], 5.0)
        self.assertEqual(a["dps_during"], 0.0)
        self.assertEqual(a["dps_lost"], 5000.0)

        formatted = dodge_timeline.format_timelines(res)
        self.assertIn("BackMs", formatted)
        self.assertIn("DmgLost", formatted)
        self.assertIn("2000ms", formatted)
        self.assertIn("5000", formatted)
        self.assertIn("median_back_ms=2000ms", formatted)
        self.assertIn("median_dps_lost=5000", formatted)

    def test_dodge_slow_return_vs_prompt_return(self) -> None:
        # Both bots have 500 DPS before cast [10000, 13000] (5s of combat since t=5000).
        # BotFast walks back into range (x=108) and casts at t=14000 (1000ms after impact).
        # BotSlow has a DoT tick at t=13010 while sitting at 42 yd (MUST NOT count as back),
        # then returns to range and casts at t=21000 (8000ms after impact).
        # BotNeverBack sits at 40 yd for 20s with a DoT tick at t=13010 -> BackMs must be None.
        traces = {
            "BotFast": [
                {"kind": "sample", "t_ms": 5000, "in_combat": True, "x": 105.0, "y": 100.0, "moving": False},
                {"kind": "event", "t_ms": 8000, "event": "damage_dealt", "value": 2500},
                {
                    "kind": "sample", "t_ms": 10000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {"kind": "event", "t_ms": 10300, "event": "act_sent", "detail": "AWAY 38"},
                {
                    "kind": "sample", "t_ms": 11500, "in_combat": True,
                    "x": 122.0, "y": 100.0, "moving": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {
                    "kind": "sample", "t_ms": 13000, "in_combat": True,
                    "x": 138.0, "y": 100.0, "moving": False,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {"kind": "sample", "t_ms": 13900, "in_combat": True, "x": 108.0, "y": 100.0, "moving": False},
                {"kind": "event", "t_ms": 14000, "event": "spell_cast", "detail": "Shadow Bolt"},
                {"kind": "sample", "t_ms": 14500, "in_combat": True, "x": 108.0, "y": 100.0, "moving": False},
            ],
            "BotSlow": [
                {"kind": "sample", "t_ms": 5000, "in_combat": True, "x": 105.0, "y": 100.0, "moving": False},
                {"kind": "event", "t_ms": 8000, "event": "damage_dealt", "value": 2500},
                {
                    "kind": "sample", "t_ms": 10000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {"kind": "event", "t_ms": 10300, "event": "act_sent", "detail": "AWAY 42"},
                {
                    "kind": "sample", "t_ms": 11500, "in_combat": True,
                    "x": 120.0, "y": 100.0, "moving": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {
                    "kind": "sample", "t_ms": 13000, "in_combat": True,
                    "x": 135.0, "y": 100.0, "moving": True,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                # DoT tick at 42 yd must NOT count as returned.
                {"kind": "event", "t_ms": 13010, "event": "damage_dealt", "value": 200},
                {"kind": "sample", "t_ms": 14000, "in_combat": True, "x": 142.0, "y": 100.0, "moving": False},
                {"kind": "sample", "t_ms": 18000, "in_combat": True, "x": 142.0, "y": 100.0, "moving": False},
                {"kind": "sample", "t_ms": 21000, "in_combat": True, "x": 107.0, "y": 100.0, "moving": False, "casting": "Shadow Bolt"},
            ],
            "BotNeverBack": [
                {"kind": "sample", "t_ms": 5000, "in_combat": True, "x": 105.0, "y": 100.0, "moving": False},
                {"kind": "event", "t_ms": 8000, "event": "damage_dealt", "value": 2500},
                {
                    "kind": "sample", "t_ms": 10000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {"kind": "event", "t_ms": 10300, "event": "act_sent", "detail": "AWAY 40"},
                {
                    "kind": "sample", "t_ms": 13000, "in_combat": True,
                    "x": 140.0, "y": 100.0, "moving": False,
                    "target_casting": "Arcane Explosion", "target_cast_radius": 34.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                # DoT tick from 40 yd while sitting out of range for 20s -> must stay None!
                {"kind": "event", "t_ms": 13010, "event": "damage_dealt", "value": 300},
                {"kind": "sample", "t_ms": 30000, "in_combat": True, "x": 140.0, "y": 100.0, "moving": False},
            ],
        }
        res = dodge_timeline.extract_dodge_timelines(traces, spell_filter="Arcane Explosion")
        bots = res["windows"][0]["bots"]
        fast, slow, never_back = bots["BotFast"], bots["BotSlow"], bots["BotNeverBack"]

        self.assertEqual(fast["dps_before"], 500.0)
        self.assertEqual(slow["dps_before"], 500.0)
        self.assertEqual(fast["time_back_in_range_ms"], 1000)
        self.assertEqual(slow["time_back_in_range_ms"], 8000)
        self.assertIsNone(never_back["time_back_in_range_ms"])
        self.assertLess(fast["time_back_in_range_ms"], slow["time_back_in_range_ms"])
        # fast: 500 * 4.0 - 0 = 2000.0; slow: 500 * 11.0 - 200 = 5300.0
        self.assertEqual(fast["dps_lost"], 2000.0)
        self.assertEqual(slow["dps_lost"], 5300.0)
        self.assertLess(fast["dps_lost"], slow["dps_lost"])

    def test_no_dodge_and_death_before_return_leave_metrics_none(self) -> None:
        traces = {
            "TankStay": [
                {"kind": "sample", "t_ms": 0, "in_combat": True, "x": 104.0, "y": 100.0, "moving": False},
                {"kind": "event", "t_ms": 5000, "event": "damage_dealt", "value": 4000},
                {
                    "kind": "sample", "t_ms": 10000, "in_combat": True,
                    "x": 104.0, "y": 100.0, "moving": False,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {
                    "kind": "sample", "t_ms": 11500, "in_combat": True,
                    "x": 104.5, "y": 100.0, "moving": False,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {
                    "kind": "sample", "t_ms": 13000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {"kind": "event", "t_ms": 13500, "event": "damage_dealt", "value": 500},
            ],
            "DodgedAndDied": [
                {"kind": "sample", "t_ms": 0, "in_combat": True, "x": 105.0, "y": 100.0, "moving": False},
                {"kind": "event", "t_ms": 5000, "event": "damage_dealt", "value": 4000},
                {
                    "kind": "sample", "t_ms": 10000, "in_combat": True,
                    "x": 105.0, "y": 100.0, "moving": False,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {"kind": "event", "t_ms": 10500, "event": "act_sent", "detail": "AWAY 35"},
                {
                    "kind": "sample", "t_ms": 11500, "in_combat": True,
                    "x": 112.0, "y": 100.0, "moving": True,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {
                    "kind": "sample", "t_ms": 13000, "in_combat": True,
                    "x": 120.0, "y": 100.0, "moving": True,
                    "target_casting": "Lightning Nova", "target_cast_radius": 30.0,
                    "target_x": 100.0, "target_y": 100.0,
                },
                {"kind": "event", "t_ms": 13200, "event": "died"},
                {"kind": "sample", "t_ms": 14000, "in_combat": False, "x": 105.0, "y": 100.0, "moving": False},
            ],
        }
        res = dodge_timeline.extract_dodge_timelines(traces, spell_filter="Lightning Nova")
        bots = res["windows"][0]["bots"]

        stay = bots["TankStay"]
        self.assertEqual(stay["dps_before"], 400.0)
        self.assertIsNone(stay["time_back_in_range_ms"])
        self.assertIsNone(stay["downtime_s"])
        self.assertIsNone(stay["dps_during"])
        self.assertIsNone(stay["dps_lost"])

        dead = bots["DodgedAndDied"]
        self.assertEqual(dead["dps_before"], 400.0)
        self.assertTrue(dead["died"])
        self.assertIsNone(dead["time_back_in_range_ms"])
        self.assertIsNone(dead["downtime_s"])
        self.assertIsNone(dead["dps_during"])
        self.assertIsNone(dead["dps_lost"])

    def test_needs_llm_and_trace_dir_for_arms_override(self) -> None:
        self.assertFalse(bench_bosses._needs_llm("mixed_all", ("no_brain", "baseline")))
        self.assertFalse(bench_bosses._needs_llm("no_brain", None))
        self.assertTrue(bench_bosses._needs_llm("mixed_all", ("llm", "no_brain")))
        self.assertTrue(bench_bosses._needs_llm("mixed_all", None))
        # Empty tuple falls back to mode just like run_boss_waves.
        self.assertTrue(bench_bosses._needs_llm("mixed_all", ()))
        self.assertFalse(bench_bosses._needs_llm("no_brain", ()))
        # Split-arm runs get distinct trace dirs so they don't clobber each other.
        d1 = bench_bosses._trace_dir_for("kaelthas_mgt", "mixed_all", ("llm", "llm_no_notes"))
        d2 = bench_bosses._trace_dir_for("kaelthas_mgt", "mixed_all", ("no_brain", "baseline"))
        self.assertNotEqual(d1, d2)
        self.assertTrue(str(d1).endswith("bench_boss_kaelthas_mgt_llm_llm_no_notes"))
        self.assertTrue(str(d2).endswith("bench_boss_kaelthas_mgt_no_brain_baseline"))
        # Multi-boss --trace-dir scopes per boss tag so earlier bosses aren't wiped.
        base = pathlib.Path("/tmp/my_traces")
        self.assertEqual(bench_bosses._resolve_trace_dir_override(base, "ikiss_blink", False), base)
        self.assertEqual(
            bench_bosses._resolve_trace_dir_override(base, "ikiss_blink", True),
            base / "ikiss_blink",
        )
        self.assertIsNone(bench_bosses._resolve_trace_dir_override(None, "ikiss_blink", True))
        # CLI rejects empty/comma-only --arms with SystemExit(2).
        orig_argv = sys.argv
        try:
            sys.argv = ["bench_bosses.py", "--arms", " , "]
            with self.assertRaises(SystemExit) as ctx:
                bench_bosses.main()
            self.assertEqual(ctx.exception.code, 2)
        finally:
            sys.argv = orig_argv

    def test_boss_notes_loaded_from_content_notes(self) -> None:
        self.assertGreaterEqual(len(bench_bosses.BOSSES), 12)
        for key, spec in bench_bosses.BOSSES.items():
            notes = spec["notes_md"]
            self.assertTrue(notes.startswith("# "), f"{key}: missing instance header")
            self.assertIn(f"## {spec['boss_name']}", notes, f"{key}: missing boss heading")
        # Unknown heading raises ValueError instead of silently returning empty notes.
        with self.assertRaises(ValueError):
            bench_bosses._load_boss_notes("PoS", "Nonexistent Boss")


if __name__ == "__main__":
    unittest.main()
