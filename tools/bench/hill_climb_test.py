#!/usr/bin/env python3
"""Unit tests for hill_climb.py & soak_scorer.py safeguards."""

import json
import os
import pathlib
import select
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
            "tools/bench/notes_hill_climb.py",
            "tools/bench/bench_bosses.py",
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
            auth = hill_climb._RelockAuth(True)
            ok, _ = hill_climb._write_lock_manifest(lf, auth=auth, approved_digest_file=df)
            self.assertTrue(ok)
            self.assertEqual(stat.S_IMODE(lf.stat().st_mode), 0o444)
            self.assertTrue(df.exists())
            self.assertEqual(hill_climb._read_approved_digest(df), hill_climb._sha256_file(lf))
            # Refuses to overwrite without force or without passphrase auth token.
            ok2, _ = hill_climb._write_lock_manifest(lf, force=False, auth=auth, approved_digest_file=df)
            self.assertFalse(ok2)
            ok2b, _ = hill_climb._write_lock_manifest(lf, force=True, auth=None, approved_digest_file=df)
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
            ok4, msg = hill_climb._verify_all_locks(lf, approved_digest_file=df)
            self.assertFalse(ok4)
            self.assertIn("missing required files", msg)

    def test_custom_lock_file_still_verifies_tracked_repo_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            lf = pathlib.Path(tmp) / "custom_scorer.sha256"
            # Write a valid manifest for the current working tree into a custom
            # path without updating config/scorer-lock.sha256.
            ok_w, _ = hill_climb._write_lock_manifest(lf, auth=hill_climb._RelockAuth(True))
            self.assertTrue(ok_w)
            # Even with a custom --lock-file path, _verify_all_locks must check
            # config/scorer-lock.sha256 unless approved_digest_file is passed.
            expected_digest = hill_climb._read_approved_digest(
                hill_climb.REPO_APPROVED_LOCK_DIGEST
            )
            actual_digest = hill_climb._sha256_file(lf)
            if expected_digest != actual_digest:
                ok_v, msg_v = hill_climb._verify_all_locks(lf)
                self.assertFalse(ok_v)
                self.assertIn("digest mismatch", msg_v)
            # Passing a mismatched tracked digest file always fails even for a
            # custom lock file path.
            fake_repo_digest = pathlib.Path(tmp) / "repo-lock.sha256"
            fake_repo_digest.write_text("f" * 64 + "  scorer.sha256\n")
            orig = hill_climb.REPO_APPROVED_LOCK_DIGEST
            try:
                hill_climb.REPO_APPROVED_LOCK_DIGEST = fake_repo_digest
                ok_v2, msg_v2 = hill_climb._verify_all_locks(lf)
                self.assertFalse(ok_v2)
                self.assertIn("digest mismatch", msg_v2)
            finally:
                hill_climb.REPO_APPROVED_LOCK_DIGEST = orig

    def test_relock_requires_external_passphrase_via_tty(self):
        import hashlib
        import pty

        # AI environment/process/cgroup detection is replaced by an external
        # passphrase file + interactive /dev/tty confirmation.
        for removed in (
            "CONFIRM_RELOCK_ENV",
            "AGENT_ENV_VARS",
            "AGENT_PROCESS_NAMES",
            "_is_agent_run",
            "_has_agent_ancestor",
            "_has_agent_cgroup",
        ):
            self.assertFalse(hasattr(hill_climb, removed))

        master_fd, slave_fd = pty.openpty()
        try:
            pts_path = os.ttyname(slave_fd)
            with open(pts_path, "r", encoding="utf-8") as check_tty:
                self.assertTrue(check_tty.isatty())
                self.assertFalse(check_tty.seekable())

            with tempfile.TemporaryDirectory() as tmp:
                tdir = pathlib.Path(tmp)
                pass_file = tdir / "relock.passphrase"

                # Missing or empty passphrase file must be refused without reading TTY.
                ok_missing, msg_missing, auth_missing = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=pts_path
                )
                self.assertFalse(ok_missing)
                self.assertIsNone(auth_missing)
                self.assertIn("missing or empty passphrase file", msg_missing)

                pass_file.write_text("   \n")
                ok_empty_file, _, _ = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=pts_path
                )
                self.assertFalse(ok_empty_file)

                secret = "correct-horse-battery-staple"
                pass_file.write_text(secret + "\n")

                # Non-TTY regular files and missing TTY paths are refused.
                reg = tdir / "not_a_tty.txt"
                reg.write_text(secret + "\n")
                ok_nontty, _, _ = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=reg
                )
                self.assertFalse(ok_nontty)
                ok_notty, _, _ = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=tdir / "missing_tty"
                )
                self.assertFalse(ok_notty)

                def drain_master() -> bytes:
                    # Drain all buffered PTY output (echoed input + prompt) across chunk boundaries.
                    chunks = []
                    while select.select([master_fd], [], [], 0.05)[0]:
                        chunk = os.read(master_fd, 4096)
                        if not chunk:
                            break
                        chunks.append(chunk)
                    return b"".join(chunks)

                # Empty input and wrong passphrase on a real TTY are refused.
                os.write(master_fd, b"\n")
                ok_blank, _, _ = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=pts_path
                )
                self.assertFalse(ok_blank)
                drain_master()

                os.write(master_fd, b"wrong-passphrase\n")
                ok_wrong, msg_wrong, auth_wrong = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=pts_path
                )
                self.assertFalse(ok_wrong)
                self.assertIsNone(auth_wrong)
                self.assertIn("passphrase mismatch", msg_wrong)
                self.assertNotIn(secret, msg_wrong)
                drain_master()

                # Matching plaintext passphrase on a real TTY succeeds.
                os.write(master_fd, (secret + "\n").encode("utf-8"))
                ok_plain, _, auth_plain = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=pts_path
                )
                self.assertTrue(ok_plain)
                self.assertIsNotNone(auth_plain)
                prompt = drain_master()
                self.assertIn(b"enter passphrase", prompt)

                # Passphrase file storing the 64-hex SHA-256 digest also works.
                pass_file.write_text(
                    hashlib.sha256(secret.encode("utf-8")).hexdigest() + "\n"
                )
                os.write(master_fd, (secret + "\n").encode("utf-8"))
                ok_hashed, _, auth_hashed = hill_climb._confirm_relock(
                    passphrase_file=pass_file, tty_path=pts_path
                )
                self.assertTrue(ok_hashed)
                self.assertIsNotNone(auth_hashed)
                drain_master()
        finally:
            os.close(slave_fd)
            os.close(master_fd)

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
