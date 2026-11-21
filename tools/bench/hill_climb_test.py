#!/usr/bin/env python3
"""Unit tests for hill_climb.py & soak_scorer.py safeguards."""

import json
import os
import pathlib
import stat
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import hill_climb  # noqa: E402
import soak_scorer  # noqa: E402


class HillClimbSafeguardsTest(unittest.TestCase):
    def test_locked_files_include_all_measurement_and_config_files(self):
        rels = {p.relative_to(ROOT).as_posix() for p in hill_climb._locked_files()}
        for required in (
            "tools/bench/soak_scorer.py",
            "tools/bench/trace_parity.py",
            "tools/bench/pairwise_ranker.py",
            "tools/bench/adversary_suite.py",
            "tools/bench/population_metrics.py",
            "tools/bench/known_bad_bots.py",
            "tools/bench/scorer_calibration.py",
            "tools/bench/blind_rate.py",
            "tools/bench/hill_climb.py",
            "tools/wlblib/cases/soak.py",
            "tools/wlblib/testing.py",
            "server_module/src/snapshot/behavior_trace.h",
            "server_module/src/snapshot/behavior_trace.cc",
            "server_module/src/snapshot/behavior_trace_scripts.h",
            "server_module/src/snapshot/behavior_trace_scripts.cc",
            "server_module/src/snapshot/snapshot_builder.cc",
            "server_module/src/snapshot/snapshot_schedule.h",
            "server_module/src/snapshot/snapshot_schedule.cc",
            "server_module/src/llm_brain_world_script.cc",
            "server_module/conf/mod-llm-brain.conf.dist",
            "server_module/src/config/llm_brain_config.cc",
            "config/llm-brain.overrides",
            "config/playerbots.overrides",
            "config/tests/soak.llm-brain.overrides",
            "config/tests/soak_horde_rfc.llm-brain.overrides",
            "config/tests/soak_alliance_dm.llm-brain.overrides",
            "config/tests/soak_northrend_hol.llm-brain.overrides",
            "config/tests/soak_heldout_uk.llm-brain.overrides",
        ):
            self.assertIn(required, rels)

    def test_lock_manifest_is_readonly_and_rejects_deleted_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = pathlib.Path(tmp)
            lf = t / "scorer.sha256"
            df = t / "scorer-lock.sha256"
            # Refuses unconfirmed writes even when the lock file does not exist yet.
            ok_unconf, _ = hill_climb._write_lock_manifest(lf, approved_digest_file=df)
            self.assertFalse(ok_unconf)
            self.assertFalse(lf.exists())
            ok, _ = hill_climb._write_lock_manifest(lf, confirmed=True, approved_digest_file=df)
            self.assertTrue(ok)
            self.assertEqual(stat.S_IMODE(lf.stat().st_mode), 0o444)
            self.assertTrue(df.exists())
            self.assertEqual(hill_climb._read_approved_digest(df), hill_climb._sha256_file(lf))
            # Refuses to overwrite without force or without confirmation.
            ok2, _ = hill_climb._write_lock_manifest(lf, force=False, confirmed=True, approved_digest_file=df)
            self.assertFalse(ok2)
            ok2b, _ = hill_climb._write_lock_manifest(lf, force=True, confirmed=False, approved_digest_file=df)
            self.assertFalse(ok2b)
            # Full manifest + repo-approved digest verify.
            ok3, _ = hill_climb._verify_all_locks(lf, approved_digest_file=df)
            self.assertTrue(ok3)
            # Mismatching or deleted approved digest fails verification.
            df.write_text("0" * 64 + "  scorer.sha256\n")
            ok_bad_df, msg_bad_df = hill_climb._verify_all_locks(lf, approved_digest_file=df)
            self.assertFalse(ok_bad_df)
            self.assertIn("digest mismatch", msg_bad_df)
            df.unlink()
            ok_no_df, msg_no_df = hill_climb._verify_all_locks(lf, approved_digest_file=df)
            self.assertFalse(ok_no_df)
            self.assertIn("digest missing", msg_no_df)
            # Deleting a line from the manifest fails verification.
            os.chmod(lf, 0o644)
            lines = lf.read_text().splitlines()
            lf.write_text("\n".join(lines[:-1]) + "\n")
            ok4, msg = hill_climb._verify_all_locks(lf)
            self.assertFalse(ok4)
            self.assertIn("missing required files", msg)

    def test_agent_ignores_custom_lock_file_and_requires_relock_confirmation(self):
        import io
        import subprocess
        import sys

        class _FakeTty(io.StringIO):
            def __init__(self, reply: str, is_tty: bool = True) -> None:
                super().__init__(reply)
                self._is_tty = is_tty

            def isatty(self) -> bool:
                return self._is_tty

            def write(self, s: str) -> int:  # discard prompt before readline
                return len(s)

        # No computable WLB_CONFIRM_RELOCK token bypass exists.
        self.assertFalse(hasattr(hill_climb, "CONFIRM_RELOCK_ENV"))

        custom = pathlib.Path("/tmp/custom_scorer_lock.sha256")
        self.assertEqual(
            hill_climb._resolve_lock_file(custom, env={"ANTIGRAVITY_AGENT": "1"}),
            hill_climb.DEFAULT_LOCK_FILE,
        )
        self.assertEqual(
            hill_climb._resolve_lock_file(custom, env={"CLAUDECODE": "1"}),
            hill_climb.DEFAULT_LOCK_FILE,
        )
        self.assertEqual(hill_climb._resolve_lock_file(custom, env={}), custom)

        token = f"RELOCK:{soak_scorer.scorer_sha256()[:12]}"
        # Even supplying a matching RELOCK:<hash12> env var AND a tty saying RELOCK
        # must be refused inside an agent run.
        ok_no_env, msg_no_env = hill_climb._confirm_relock(
            env={"ANTIGRAVITY_AGENT": "1"},
            tty_opener=lambda: _FakeTty("RELOCK\n"),
        )
        self.assertFalse(ok_no_env)
        self.assertNotIn("RELOCK:", msg_no_env)
        self.assertNotIn(soak_scorer.scorer_sha256()[:8], msg_no_env)

        ok_tok, msg_tok = hill_climb._confirm_relock(
            env={"ANTIGRAVITY_AGENT": "1", "WLB_CONFIRM_RELOCK": token},
            tty_opener=lambda: _FakeTty("RELOCK\n"),
        )
        self.assertFalse(ok_tok)
        self.assertNotIn("RELOCK:", msg_tok)

        # Non-agent runs require an interactive /dev/tty typing exact 'RELOCK'
        # (env tokens, non-tty streams, and missing /dev/tty are all rejected).
        ok_env_only, _ = hill_climb._confirm_relock(
            env={"WLB_CONFIRM_RELOCK": token},
            tty_opener=lambda: _FakeTty("\n"),
        )
        self.assertFalse(ok_env_only)
        ok_nontty, _ = hill_climb._confirm_relock(
            env={},
            tty_opener=lambda: _FakeTty("RELOCK\n", is_tty=False),
        )
        self.assertFalse(ok_nontty)

        def _raise_no_tty():
            raise OSError("no controlling terminal")

        ok_notty, _ = hill_climb._confirm_relock(env={}, tty_opener=_raise_no_tty)
        self.assertFalse(ok_notty)
        ok_human, _ = hill_climb._confirm_relock(
            env={},
            tty_opener=lambda: _FakeTty("RELOCK\n", is_tty=True),
        )
        self.assertTrue(ok_human)

        # Synthetic /proc tree: unsetting env vars, spaces/parens in comm, and
        # setsid/double-fork reparenting to pid 1 under an agent cgroup must
        # still be caught.
        with tempfile.TemporaryDirectory() as tmp:
            proc_root = pathlib.Path(tmp)
            for pid, ppid, comm, cmd, cgroup in (
                (99, 1, "agy", b"/home/user/.gemini/bin/agy\x00remote-control\x00serve\x00", "0::/user.slice\n"),
                (100, 99, "my (worker)", b"python3\x00worker.py\x00", "0::/user.slice\n"),
                (
                    200,
                    1,
                    "python3",
                    b"python3\x00-c\x00...",
                    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/antigravity-cli-daemon.service\n",
                ),
                (50, 1, "bash", b"/bin/bash\x00", "0::/user.slice/user-1000.slice/session-1.scope\n"),
            ):
                d = proc_root / str(pid)
                d.mkdir()
                (d / "stat").write_text(f"{pid} ({comm}) S {ppid} 1 1 0 0 0 0\n")
                (d / "cmdline").write_bytes(cmd)
                (d / "cgroup").write_text(cgroup)
            (proc_root / "self").symlink_to("100")
            self.assertTrue(hill_climb._has_agent_ancestor(proc_root))
            self.assertTrue(hill_climb._is_agent_run(env={}, proc_root=proc_root))
            (proc_root / "self").unlink()
            (proc_root / "self").symlink_to("200")
            self.assertTrue(hill_climb._has_agent_ancestor(proc_root))
            self.assertEqual(
                hill_climb._resolve_lock_file(custom, env={}, proc_root=proc_root),
                hill_climb.DEFAULT_LOCK_FILE,
            )
            ok_reparented, _ = hill_climb._confirm_relock(
                env={},
                proc_root=proc_root,
                tty_opener=lambda: _FakeTty("RELOCK\n"),
            )
            self.assertFalse(ok_reparented)
            (proc_root / "self").unlink()
            (proc_root / "self").symlink_to("50")
            self.assertFalse(hill_climb._has_agent_ancestor(proc_root))
            self.assertFalse(hill_climb._is_agent_run(env={}, proc_root=proc_root))

        # Live host /proc check with all env vars cleared via `env -i`, plus
        # double-fork + os.setsid() reparenting to pid 1 when in an agent cgroup.
        expected_ancestor = hill_climb._has_agent_ancestor()
        res = subprocess.run(
            [
                "env",
                "-i",
                f"PATH={os.environ.get('PATH', '/usr/bin')}",
                sys.executable,
                "-c",
                "import sys; sys.path.insert(0, 'tools/bench'); import hill_climb; "
                "print(int(hill_climb._is_agent_run()))",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(res.stdout.strip(), str(int(expected_ancestor)))

        if hill_climb._has_agent_cgroup(pathlib.Path("/proc/self")):
            setsid_code = (
                "import os, sys; "
                "sys.path.insert(0, 'tools/bench'); "
                "import hill_climb; "
                "r, w = os.pipe(); "
                "p1 = os.fork()\n"
                "if p1 > 0:\n"
                "  os.close(w); os.waitpid(p1, 0); "
                "  out = os.read(r, 16).decode().strip(); os.close(r); print(out)\n"
                "else:\n"
                "  os.setsid(); p2 = os.fork()\n"
                "  if p2 > 0: os._exit(0)\n"
                "  os.close(r); os.write(w, str(int(hill_climb._is_agent_run())).encode()); "
                "  os.close(w); os._exit(0)\n"
            )
            res2 = subprocess.run(
                [
                    "env",
                    "-i",
                    f"PATH={os.environ.get('PATH', '/usr/bin')}",
                    sys.executable,
                    "-c",
                    setsid_code,
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertEqual(res2.stdout.strip(), "1")

    def test_has_human_traces_rejects_bot_rows_and_requires_min_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            self.assertFalse(hill_climb._has_human_traces(d))
            (d / "human" / "Alice").mkdir(parents=True)
            p = d / "human" / "Alice" / "2026-11-19.jsonl"
            # Bot-role traces copied into human/ must NOT count as human.
            bot_line = json.dumps({"kind": "sample", "role": "bot", "t_ms": 1000})
            p.write_text((bot_line + "\n") * soak_scorer.MIN_HUMAN_SAMPLES)
            self.assertFalse(hill_climb._has_human_traces(d))
            # 300 junk rows with no positions/movement metrics must NOT pass.
            junk_line = json.dumps({"kind": "sample", "role": "human", "t_ms": 1000})
            p.write_text((junk_line + "\n") * soak_scorer.MIN_HUMAN_SAMPLES)
            self.assertFalse(hill_climb._has_human_traces(d))
            # 1 Hz traces must be rejected by the 5 Hz source-rate parity check.
            old_1hz_rows = [
                json.dumps({
                    "kind": "sample", "role": "human", "name": "Alice",
                    "t_ms": 1000 * i, "map": 1, "x": float(i * 4), "y": 0.0, "z": 0.0,
                    "o": 0.0, "moving": (i % 5 != 0), "jumping": False,
                    "in_combat": False, "dead": False, "in_town": False,
                })
                for i in range(1, soak_scorer.MIN_HUMAN_SAMPLES + 1)
            ]
            p.write_text("\n".join(old_1hz_rows) + "\n")
            self.assertFalse(hill_climb._has_human_traces(d))
            # Reaching MIN_HUMAN_SAMPLES real 5 Hz movement rows (>= 60 s) passes.
            real_rows = [
                json.dumps({
                    "kind": "sample", "role": "human", "name": "Alice",
                    "t_ms": 200 * i, "map": 1, "x": float(i * 1.4), "y": 0.0, "z": 0.0,
                    "o": 0.0, "moving": (i % 25 != 0), "jumping": False,
                    "in_combat": False, "dead": False, "in_town": False,
                })
                for i in range(1, max(soak_scorer.MIN_HUMAN_SAMPLES, 350) + 1)
            ]
            p.write_text("\n".join(real_rows) + "\n")
            self.assertTrue(hill_climb._has_human_traces(d))

    def test_edit_scope_allows_humanization_and_rejects_game_logic(self):
        ok, bad = hill_climb._check_edit_scope([
            "server_module/src/options/human_pacing.cc",
            "content/RULES.md",
            "content/identities/default.md",
        ])
        self.assertTrue(ok)
        self.assertEqual(bad, [])

        ok, bad = hill_climb._check_edit_scope([
            "content/RULES.md",
            "config/scorer-lock.sha256",
            "server_module/src/options/option_executor.cc",
            "server_module/src/llm_brain_world_script.cc",
        ])
        self.assertFalse(ok)
        self.assertEqual(
            bad,
            [
                "config/scorer-lock.sha256",
                "server_module/src/options/option_executor.cc",
                "server_module/src/llm_brain_world_script.cc",
            ],
        )

    def test_fixed_guardrails_read_nested_progress_dict(self):
        # Matches real `soak_scorer.score_soak()` / `soak.run_soak()` result shape.
        real_shape = {
            "score": 75.0,
            "guardrails_ok": True,
            "progress": {"xp_per_hour": 5000.0, "deaths": 2, "total_xp": 8000},
        }
        ok, fails = hill_climb._check_fixed_guardrails(
            real_shape, min_xp_per_hour=3000.0, max_deaths=4
        )
        self.assertTrue(ok)
        self.assertEqual(fails, [])

        bad_shape = {
            "score": 75.0,
            "guardrails_ok": True,
            "progress": {"xp_per_hour": 2500.0, "deaths": 5, "total_xp": 4000},
        }
        ok, fails = hill_climb._check_fixed_guardrails(
            bad_shape, min_xp_per_hour=3000.0, max_deaths=4
        )
        self.assertFalse(ok)
        self.assertEqual(len(fails), 2)

    def test_retired_composite_score_is_removed(self):
        self.assertFalse(hasattr(soak_scorer, "FALLBACK_TARGETS"))
        self.assertFalse(hasattr(soak_scorer, "METRIC_WEIGHTS"))
        self.assertFalse(hasattr(soak_scorer, "_score_distributions"))


if __name__ == "__main__":
    unittest.main()
