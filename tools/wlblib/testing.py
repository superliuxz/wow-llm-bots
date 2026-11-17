"""`wlb test`: the four test targets.

  unit   the gtest suites (common, module, harness); no game, no GPU. `ninja unit_tests`.
  e2e    live scenarios against a real worldserver (test build + a test config), pass or fail
  perf   what things cost, each checked against a bound, so a regression fails
  eval   how well the brain decides, on saved snapshots, scored

A case is a module in wlblib/cases/ named <target>_<name>.py with
  NAME         short name, as `wlb test e2e NAME` selects it
  DESCRIPTION  one line
  run(env)     does the case; raises (expect() fails with a message) when it fails
and, for e2e, TEST_CONFIG (config/tests/<TEST_CONFIG>.*.overrides) and BOT (the character to wait
for). The runner prints one PASS or FAIL line per case and exits 1 if any failed.
"""

import datetime
import importlib
import json
import os
import pathlib
import pkgutil
import shutil
import subprocess
import time
import urllib.request

from wlblib import build, cases, server
from wlblib.common import LOGS, OUT, ROOT, Failed, output, run, running_pid, say

TARGETS = ("unit", "e2e", "perf", "eval")


def expect(condition, message):
    """Fails the case with `message` unless `condition`."""
    if not condition:
        raise Failed(message)


def discover(target):
    """The cases of a target, by name."""
    found = {}
    for info in pkgutil.iter_modules(cases.__path__):
        if info.name.startswith(f"{target}_"):
            module = importlib.import_module(f"wlblib.cases.{info.name}")
            found[module.NAME] = module
    return dict(sorted(found.items()))


def list_cases():
    for target in ("e2e", "perf"):
        found = discover(target)
        say(f"{target}: " + (", ".join(found) if found else "(no cases yet)"))
    from wlblib.cases import eval_plans
    say("eval: " + ", ".join(eval_plans.case_names()))
    say("unit: every gtest suite (wlb test unit [--filter PATTERN] [--variant asan|tsan|msan])")


def unit(variant="default", gtest_filter=None):
    build.common(variant, tests=True, gtest_filter=gtest_filter)
    say(f"PASS unit ({variant}{', filter ' + gtest_filter if gtest_filter else ''})")


def _live_harness_pids():
    """Pids of this checkout's harness binary (out/common/harness) that are running."""
    binary = (OUT / "common" / "harness").resolve()
    pids = []
    for proc in pathlib.Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            # A binary rebuilt since it started reads "<path> (deleted)".
            exe = os.readlink(proc / "exe").removesuffix(" (deleted)")
            if pathlib.Path(exe) == binary:
                pids.append(int(proc.name))
        except OSError:
            continue
    return pids


# ---- e2e ----------------------------------------------------------------------------------------

class Stack:
    """A running test stack: the test build of the server with a test config, the test bot
    online, and a record of the server log from the start of the run.

    It is the same machine and database as the play server (an isolated stack with its own
    ports and a restored database is planned, docs/plans/phase2.md section E), so it stops the
    play server first and puts it back afterwards: same build, normal config, started again if it
    was running. Test characters are recreated from fixtures on every start; yours isn't touched."""

    def __init__(self, test_config, bot, keep=False, online=1):
        self.test_config, self.bot, self.keep, self.online = test_config, bot, keep, online
        self.was_running = False
        self.flavor_before = None
        self.log_start = 0

    def __enter__(self):
        # A live harness reconnects within a second and takes the socket from the scripted
        # brain (the module keeps the newest client), so a test can't share it.
        harness = _live_harness_pids()
        if harness:
            raise Failed(f"a live harness is running (pid {', '.join(map(str, harness))}): "
                         "stop it first (it would take the test's socket)")
        self.was_running = running_pid("worldserver") is not None
        self.flavor_before = build.built_flavor()
        try:
            if self.was_running:
                say("stopping the play server")
                server.server_stop()
            if build.built_flavor() != "test":
                say("building the test flavor of the server (the module only)")
                build.server(test=True)
            server.config(self.test_config)
            server.server_start()
            server.wait_online(self.bot)
            if self.online > 1:
                server.wait_online_count(self.online)
            try:
                self.log_start = (LOGS / "Server.log").stat().st_size
            except OSError:
                self.log_start = 0
            # The first snapshots are sent as soon as a harness connects.
        except BaseException:
            self.restore()
            raise
        return self

    def __exit__(self, *exc):
        if not self.keep:
            self.restore()
        else:
            say("leaving the test stack running (--keep-server)")
        return False

    def restore(self):
        say("restoring the play server")
        server.server_stop()
        if self.flavor_before and build.built_flavor() != self.flavor_before:
            build.server(test=self.flavor_before == "test")
        server.config()
        if self.was_running:
            server.server_start()

    def server_log(self):
        """The module's log lines since the stack came up."""
        try:
            with open(LOGS / "Server.log", "rb") as f:
                f.seek(self.log_start)
                return f.read().decode(errors="replace")
        except OSError:
            return ""

    def play(self, words, seconds=90, tail_s=15):
        """Runs the scripted brain (tools/snapshot/fake_harness.py --choose) against the module
        and returns its transcript: snapshots, events, commands sent, words never offered. It
        stops `tail_s` after the last word was sent, or after `seconds`."""
        if not (OUT / "common" / "libwlb_wire_py.so").exists():
            build.common("default", tests=False)
        result = subprocess.run(
            ["python3", "-I", ROOT / "tools/snapshot/fake_harness.py", "--choose", "--json",
             f"--tail={tail_s}", str(seconds), *words], capture_output=True, text=True)
        if result.returncode != 0:
            raise Failed(f"the scripted brain failed:\n{result.stderr.strip()[-600:]}")
        return json.loads(result.stdout)

    def whisper(self, sender, receiver, text, reply, seconds=60):
        """Runs tools/snapshot/fake_harness.py --whisper: `sender` whispers `text` to `receiver`,
        which whispers `reply` back. Returns its transcript (what each bot heard)."""
        if not (OUT / "common" / "libwlb_wire_py.so").exists():
            build.common("default", tests=False)
        result = subprocess.run(
            ["python3", "-I", ROOT / "tools/snapshot/fake_harness.py", "--whisper", "--json",
             str(seconds), sender, receiver, text, reply], capture_output=True, text=True)
        if result.returncode != 0:
            raise Failed(f"the scripted brain failed:\n{result.stderr.strip()[-600:]}")
        return json.loads(result.stdout)


def _select(target, names):
    found = discover(target)
    unknown = [n for n in names if n not in found]
    if unknown:
        raise Failed(f"no {target} case {', '.join(unknown)} (have: {', '.join(found) or 'none'})")
    selected = [found[n] for n in names] if names else list(found.values())
    if not selected:
        raise Failed(f"no {target} cases yet")
    return selected


def _run_selected(selected, keep_server, call):
    """Runs `call(module, stack)` for every case. Cases with a TEST_CONFIG share one stack per
    (TEST_CONFIG, BOT, ONLINE); cases without one run with stack None, first (the play server
    stays up for them)."""
    groups, plain, results = {}, [], []
    for module in selected:
        config = getattr(module, "TEST_CONFIG", None)
        if config:
            key = (config, module.BOT, getattr(module, "ONLINE", 1))
            groups.setdefault(key, []).append(module)
        else:
            plain.append(module)
    for module in plain:
        results.append(call(module, None))
    for (config, bot, online), modules in groups.items():
        with Stack(config, bot, keep=keep_server, online=online) as stack:
            for module in modules:
                results.append(call(module, stack))
    _summary(results)


def e2e(names=(), keep_server=False):
    selected = _select("e2e", names)
    missing = [m.NAME for m in selected if not getattr(m, "TEST_CONFIG", None)]
    if missing:
        raise Failed(f"e2e cases need a TEST_CONFIG: {', '.join(missing)}")
    _run_selected(selected, keep_server, lambda module, stack: _run_case("e2e", module, stack))


# ---- perf ---------------------------------------------------------------------------------------

BASELINES = pathlib.Path(__file__).resolve().parent / "cases" / "perf_baselines.json"


class Baselines:
    """What each perf measurement was when it was last accepted (`--update-baselines`):
    {"_meta": {...}, "<case>": {"<metric>": value}}. Machine-specific, tracked in git so a
    regression shows against the numbers of the same box."""

    def __init__(self):
        try:
            self.data = json.loads(BASELINES.read_text())
        except (OSError, ValueError):
            self.data = {}

    def get(self, case, key):
        return self.data.get(case, {}).get(key)

    def record(self, case, measured):
        self.data[case] = dict(sorted(measured.items()))

    def save(self):
        self.data["_meta"] = {"recorded": datetime.date.today().isoformat(), **_machine()}
        BASELINES.write_text(json.dumps(self.data, indent=2, sort_keys=True) + "\n")


def _machine():
    def first_line(cmd):
        try:
            return output(cmd).splitlines()[0].strip()
        except (Failed, IndexError, OSError):
            return "unknown"
    cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo")
                if l.startswith("model name")), "unknown")
    return {"cpu": cpu, "gpu": first_line(
        ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"])}


class Perf:
    """What a perf case gets: `stack` (the running test stack, or None) and `measure()`."""

    def __init__(self, case, stack, baselines):
        self.case, self.stack, self.baselines = case, stack, baselines
        self.measured, self.shown, self.failures = {}, [], []

    def measure(self, key, value, unit="", *, lower_is_better=True, factor=1.5, slack=0.0,
                limit=None):
        """Records `value` and checks it. Against the baseline: worse than the baseline by more
        than `factor` fails, with `slack` (in the metric's own unit) allowed besides, so a tiny
        baseline doesn't fail on noise. Against `limit` (a hard bound, e.g. a documented
        criterion): beyond it fails whatever the baseline says. No baseline yet: it passes, and
        says so (record one with `wlb test perf --update-baselines`)."""
        self.measured[key] = value
        base = self.baselines.get(self.case, key)
        text = f"{key}={value:.4g}{unit}"
        if base is None:
            text += " (no baseline)"
        else:
            text += f" (baseline {base:.4g})"
            if lower_is_better:
                allowed = max(base * factor, base + slack)
                if value > allowed:
                    self.failures.append(f"{key} {value:.4g}{unit} is worse than the baseline "
                                         f"{base:.4g} (allowed {allowed:.4g})")
            else:
                allowed = min(base / factor, base - slack)
                if value < allowed:
                    self.failures.append(f"{key} {value:.4g}{unit} is worse than the baseline "
                                         f"{base:.4g} (allowed {allowed:.4g})")
        if limit is not None and (value > limit if lower_is_better else value < limit):
            self.failures.append(f"{key} {value:.4g}{unit} is beyond the limit {limit:.4g}")
        self.shown.append(text)

    def summary(self):
        return ", ".join(self.shown)


def perf(names=(), keep_server=False, update_baselines=False, verify=False):
    """Runs the perf cases against their baselines. With `update_baselines`, each case's numbers
    are recorded as its baseline; with `verify` as well, the case then runs a second time in the
    same session (the same stack, the same llama-server) and is checked against what was just
    recorded: a measurement that can't reproduce itself has a baseline that can't be trusted."""
    selected = _select("perf", names)
    baselines = Baselines()

    def call(module, stack):
        ok = _run_perf(module, stack, baselines, update_baselines)
        if update_baselines and verify:
            say(f"verifying {module.NAME} against the baseline just recorded")
            return _run_perf(module, stack, baselines, False)
        return ok

    _run_selected(selected, keep_server, call)


def _run_perf(module, stack, baselines, update):
    start = time.time()
    p = Perf(module.NAME, stack, baselines)
    crashed = False
    try:
        module.run(p)
        if p.failures:
            raise Failed("; ".join(p.failures))
        ok, detail = True, p.summary()
    except Failed as e:
        ok, detail = False, f"{e} [{p.summary()}]"
    except Exception as e:
        ok, detail, crashed = False, f"{type(e).__name__}: {e}", True
    if update and p.measured and not crashed:
        baselines.record(module.NAME, p.measured)
        baselines.save()
        detail += " [baseline recorded]"
    say(f"{'PASS' if ok else 'FAIL'} perf {module.NAME} ({time.time() - start:.1f} s): {detail}")
    return ok


class LlamaServer:
    """llama-server with the brain's model (`wlb llm`), for the length of a `with`. One that is
    already running is used as it is and left running (unless a different `model` was asked for)."""

    URL = "http://127.0.0.1:8090/health"
    MODELS_URL = "http://127.0.0.1:8090/v1/models"

    def __init__(self, slots=16, timeout_s=240, model=None):
        self.slots, self.timeout_s, self.model, self.proc = slots, timeout_s, model, None

    def healthy(self):
        try:
            with urllib.request.urlopen(self.URL, timeout=2) as r:
                return r.status == 200
        except OSError:
            return False

    def loaded_model_matches(self):
        if not self.model:
            return True
        try:
            with urllib.request.urlopen(self.MODELS_URL, timeout=2) as r:
                body = r.read().decode(errors="replace")
                return self.model in body
        except OSError:
            return False

    def __enter__(self):
        if self.healthy():
            if not self.loaded_model_matches():
                raise Failed(f"another llama-server is already running on :8090 "
                             f"(stop it first to load {self.model})")
            say("using the llama-server that is already running")
            return self
        label = self.model or "the 35B model"
        say(f"starting llama-server ({label})")
        (OUT / "logs").mkdir(parents=True, exist_ok=True)
        log = open(OUT / "logs" / "llama-perf.log", "wb")
        cmd = [ROOT / "tools" / "wlb", "llm", "--slots", str(self.slots)]
        if self.model:
            cmd += ["--model", self.model]
        self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.time() + self.timeout_s
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise Failed("llama-server exited (see out/logs/llama-perf.log)")
            if self.healthy():
                return self
            time.sleep(2)
        self.__exit__()
        raise Failed(f"llama-server not healthy after {self.timeout_s} s")

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        return False


def capture_snapshots(seconds=300):
    """Records live snapshots of 50 questing bots (the `questing` test stack) into
    out/bench/snapshots/quest, the input of the brain load replay (tools/bench/brain_load.py).
    Replaces what is there."""
    target = OUT / "bench" / "snapshots" / "quest"
    if not (OUT / "common" / "libwlb_wire_py.so").exists():
        build.common("default", tests=False)
    with Stack("questing", "Loada", online=50) as stack:
        shutil.rmtree(target, ignore_errors=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        say(f"capturing {seconds} s of snapshots into {target.relative_to(ROOT)}")
        run(["python3", "-I", ROOT / "tools/bench/capture_snapshots.py", target, str(seconds)])
    say(f"captured {sum(1 for _ in target.rglob('*.json'))} snapshots")


def eval_(names=(), model=None, shootout=False):
    from wlblib.cases import eval_plans

    cases = eval_plans.load_cases(names)
    if shootout:
        models = [m for m in eval_plans.SHOOTOUT_MODELS if (ROOT / "models" / m).exists()]
        if not models:
            raise Failed("no shootout models found in models/")
        runs = []
        for m in models:
            with LlamaServer(slots=4, model=m):
                runs.append(eval_plans.run_suite(cases, label=m))
        say("== shootout summary ==")
        header = f"  {'case':25s} | " + " | ".join(f"{r['label'][:18]:18s}" for r in runs)
        say(header)
        for i, case in enumerate(cases):
            cells = []
            for r in runs:
                row = r["rows"][i]
                step = row["got"].get("plan_desc", "ERR")[:13]
                cells.append(f"{'PASS' if row['ok'] else 'FAIL'} ({step:11s})"[:18].ljust(18))
            say(f"  {case['name']:25s} | " + " | ".join(cells))
        totals = " | ".join(
            f"{r['passed']:2d}/{r['total']:<2d} ({r['median_ms']:4.0f} ms)   "[:18] for r in runs)
        say(f"  {'SCORE':25s} | {totals}")
        return

    with LlamaServer(slots=4, model=model):
        res = eval_plans.run_suite(cases, label=model)
    if res["passed"] != res["total"]:
        raise Failed(f"{res['total'] - res['passed']} of {res['total']} eval cases failed")


# ---- shared -------------------------------------------------------------------------------------

def _run_case(target, module, stack):
    start = time.time()
    try:
        detail = module.run(stack) or ""
        ok = True
    except Failed as e:
        ok, detail = False, str(e)
    except Exception as e:  # A bug in the case or the tools: also a failure, with the cause.
        ok, detail = False, f"{type(e).__name__}: {e}"
    seconds = time.time() - start
    say(f"{'PASS' if ok else 'FAIL'} {target} {module.NAME} ({seconds:.1f} s)"
        + (f": {detail}" if detail else ""))
    return ok


def _summary(results):
    passed = sum(results)
    say(f"{passed} of {len(results)} passed")
    if passed != len(results):
        raise Failed(f"{len(results) - passed} failed")
