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
import pathlib
import pkgutil
import select
import shutil
import socket
import subprocess
import threading
import time

from wlblib import build, cases, llm, server, wire
from wlblib.common import OUT, PLAY, ROOT, TEST, Failed, output, run, running_pid, say

TARGETS = ("unit", "e2e", "perf", "eval", "soak")


def expect(condition, message):
    """Fails the case with `message` unless `condition`."""
    if not condition:
        raise Failed(message)


def send_cmd(sock, cmd):
    """Encodes and sends one length-prefixed BEVE Command frame over `sock`."""
    wire.send_frame(sock, wire.encode_command(cmd))


def find_option(options, prefix, avoid=None):
    """1-based option index whose text starts with `prefix` (and skips `avoid`)."""
    return next(
        (i for i, o in enumerate(options, 1)
         if o.get("text", "").startswith(prefix)
         and not (avoid and avoid in o.get("text", ""))),
        None,
    )


class ProxyRecorder:
    """Forwards BEVE frames between `harness run` and `mod-llm-brain` while
    recording decoded snapshots, events, visibility errors, and commands."""

    def __init__(self, real_sock: pathlib.Path, proxy_sock: pathlib.Path):
        self.real_sock = real_sock
        self.proxy_sock = proxy_sock
        self.lock = threading.Lock()
        self.snapshots = []
        self.events = []
        self.commands = []
        self.visibility_errors = []
        self._stop = threading.Event()
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        proxy_sock.unlink(missing_ok=True)
        self._listener.bind(str(proxy_sock))
        self._listener.listen(2)
        self._listener.settimeout(0.2)
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        try:
            self._listener.close()
        except OSError:
            pass
        self._thread.join(timeout=5)
        self.proxy_sock.unlink(missing_ok=True)

    def _serve(self):
        client = None
        upstream = None
        try:
            while not self._stop.is_set():
                try:
                    client, _ = self._listener.accept()
                    break
                except socket.timeout:
                    continue
            if client is None:
                return
            upstream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            upstream.connect(str(self.real_sock))
            in_mod = wire.FrameReader()
            in_cmd = wire.FrameReader()
            while not self._stop.is_set():
                r, _, _ = select.select([client, upstream], [], [], 0.1)
                now = time.monotonic()
                if upstream in r:
                    data = upstream.recv(1 << 20)
                    if not data:
                        break
                    client.sendall(data)
                    for payload in in_mod.feed(data):
                        verr = wire.visibility_errors(payload)
                        msg = wire.decode_module_message(payload)
                        with self.lock:
                            if verr is None:
                                if len(self.visibility_errors) < 10:
                                    self.visibility_errors.append(
                                        "failed to decode module frame for visibility check"
                                    )
                            elif verr and len(self.visibility_errors) < 10:
                                self.visibility_errors.extend(verr[:5])
                            if msg and "snapshot" in msg:
                                self.snapshots.append((now, msg["snapshot"]))
                            elif msg and "event" in msg:
                                self.events.append((now, msg["event"]))
                if client in r:
                    data = client.recv(1 << 20)
                    if not data:
                        break
                    upstream.sendall(data)
                    for payload in in_cmd.feed(data):
                        cmd = wire.decode_command(payload)
                        if cmd:
                            with self.lock:
                                self.commands.append((now, cmd))
        finally:
            for s in (client, upstream):
                if s is not None:
                    try:
                        s.close()
                    except OSError:
                        pass

    def wait_until(self, pred, timeout_s=30.0, desc="condition"):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            with self.lock:
                val = pred(self)
                if val:
                    return val
            time.sleep(0.05)
        raise AssertionError(f"timed out ({timeout_s}s) waiting for {desc}")


def discover(target):
    """The cases of a target, by name (supports both single-case and grouped `CASES` modules)."""
    found = {}
    for info in pkgutil.iter_modules(cases.__path__):
        if info.name.startswith(f"{target}_"):
            module = importlib.import_module(f"wlblib.cases.{info.name}")
            if hasattr(module, "CASES"):
                for case in module.CASES:
                    found[case.NAME] = case
            else:
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


# ---- e2e ----------------------------------------------------------------------------------------

class Stack:
    """The running test stack: the isolated test server (common.TEST: its own build, port, run
    directory, logs and databases) with a test config and the test bot online, and a record of
    its log from the start of the run. Your play server keeps running meanwhile, untouched.

    Every run starts from the same state: the test server's databases are reset from their
    golden copy (server.test_databases_reset), and test characters are recreated from fixtures
    on every start."""

    socket = TEST.run / "brain.sock"

    def __init__(self, test_config, bot, keep=False, online=1):
        self.test_config, self.bot, self.keep, self.online = test_config, bot, keep, online
        self.log_start = 0

    def __enter__(self):
        build.server(test=True)  # Only what changed since the last run.
        server.server_stop(inst=TEST)  # One left running by --keep-server or a crash.
        try:
            server.test_databases_reset()
            self._bring_up()
        except BaseException:
            server.server_stop(inst=TEST)
            raise
        return self

    def _bring_up(self):
        server.config(self.test_config, inst=TEST)
        server.server_start(TEST)
        server.wait_online(self.bot, inst=TEST)
        if self.online > 1:
            server.wait_online_count(self.online, inst=TEST)
        try:
            self.log_start = (TEST.logs / "Server.log").stat().st_size
        except OSError:
            self.log_start = 0
        # The first snapshots are sent as soon as a harness connects.

    def switch(self, test_config, bot, online=1):
        """Another test config on the same test server: a config and a restart."""
        self.test_config, self.bot, self.online = test_config, bot, online
        say(f"switching the test stack to {test_config}")
        try:
            server.server_stop(inst=TEST)
            self._bring_up()
        except BaseException:
            if not self.keep:
                server.server_stop(inst=TEST)
            raise

    def __exit__(self, *exc):
        if self.keep:
            say("leaving the test server running (--keep-server; stop it with "
                "`tools/wlb server stop --test`)")
        else:
            server.server_stop(inst=TEST)
        return False

    def server_log(self):
        """The module's log lines since the stack came up."""
        try:
            with open(TEST.logs / "Server.log", "rb") as f:
                f.seek(self.log_start)
                return f.read().decode(errors="replace")
        except OSError:
            return ""

    def _check_transcript(self, transcript):
        if transcript.get("visibility_errors"):
            raise Failed("the module sent what the bot can't see:\n  " +
                         "\n  ".join(transcript["visibility_errors"]))
        return transcript

    def play(self, words, seconds=90, tail_s=15):
        """Runs the scripted brain (`wlblib.wire.play_script`) against the module."""
        if not (OUT / "common" / "libwlb_wire_py.so").exists():
            build.common("default", tests=False)
        return self._check_transcript(
            wire.play_script(str(self.socket), seconds, words, echo=lambda _line: None,
                             tail_s=tail_s)
        )

    def whisper(self, sender, receiver, text, reply, seconds=60):
        """Runs `wlblib.wire.whisper_script`: `sender` whispers `text` to `receiver`."""
        if not (OUT / "common" / "libwlb_wire_py.so").exists():
            build.common("default", tests=False)
        return self._check_transcript(
            wire.whisper_script(str(self.socket), seconds, sender, receiver, text, reply,
                                echo=lambda _line: None)
        )


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
    # One test stack for the whole run: the test build is installed once and the play server
    # restored once; between configs only the config changes (a restart).
    if groups:
        keys = list(groups)
        config, bot, online = keys[0]
        with Stack(config, bot, keep=keep_server, online=online) as stack:
            for i, key in enumerate(keys):
                if i:
                    stack.switch(*key)
                for module in groups[key]:
                    results.append(call(module, stack))
    _summary(results)


def e2e(names=(), keep_server=False):
    build.common("default", tests=False)
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
        merged = dict(self.data.get(case, {}))
        merged.update(measured)
        self.data[case] = dict(sorted(merged.items()))

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
        # Worse than the baseline (a warning beside a busy play server), and beyond a limit.
        self.measured, self.shown, self.failures, self.beyond = {}, [], [], []

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
            self.beyond.append(f"{key} {value:.4g}{unit} is beyond the limit {limit:.4g}")
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
    busy = _play_busy()
    if busy:
        say(f"the play server is running with {busy} online: it competes for the CPU and GPU, "
            "so a regression against the baseline is a warning this run, not a failure")
        if update_baselines:
            raise Failed("not recording baselines while the play server is busy (stop it, or "
                         "run without --update-baselines)")

    def call(module, stack):
        ok = _run_perf(module, stack, baselines, update_baselines, busy)
        if update_baselines and verify:
            say(f"verifying {module.NAME} against the baseline just recorded")
            return _run_perf(module, stack, baselines, False, busy)
        return ok

    _run_selected(selected, keep_server, call)


def _play_busy():
    """"N characters" when the play server is running with characters online, else None."""
    if not running_pid("worldserver", PLAY):
        return None
    try:
        online = len(server.online_characters(PLAY))
    except Failed:
        return None
    return f"{online} characters" if online else None


def _run_perf(module, stack, baselines, update, busy=None):
    start = time.time()
    p = Perf(module.NAME, stack, baselines)
    crashed = False
    try:
        module.run(p)
        if p.failures and busy:
            # Measured beside a busy play server: reported, not failed.
            say(f"WARN perf {module.NAME}: {'; '.join(p.failures)} (play server busy)")
        elif p.failures and update:
            say(f"WARN perf {module.NAME}: {'; '.join(p.failures)} (updating baseline)")
        if p.beyond or (p.failures and not busy and not update):
            raise Failed("; ".join(p.beyond + ([] if (busy or update) else p.failures)))
        ok, detail = True, p.summary()
    except Failed as e:
        ok, detail = False, f"{e} [{p.summary()}]"
    except Exception as e:
        ok, detail, crashed = False, f"{type(e).__name__}: {e}", True
    if update and ok and p.measured and not crashed:
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
        return llm.health_check()

    def loaded_model_matches(self):
        return llm.loaded_model_matches(self.model)

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
    Replaces what is there only after a good capture."""
    target = OUT / "bench" / "snapshots" / "quest"
    tmp = target.with_name(target.name + ".tmp")
    if not (OUT / "common" / "libwlb_wire_py.so").exists():
        build.common("default", tests=False)
    with Stack("questing", "Loada", online=50) as stack:
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True, exist_ok=True)
        say(f"capturing {seconds} s of snapshots from {stack.socket} into {target.relative_to(ROOT)}")
        try:
            rc = wire.capture_snapshots(tmp, seconds=seconds, sock_path=str(stack.socket))
            if rc != 0:
                raise Failed("capture_snapshots failed")
        except Exception:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
    count = sum(1 for _ in tmp.rglob("*.json"))
    if count == 0:
        shutil.rmtree(tmp, ignore_errors=True)
        raise Failed("captured 0 snapshots; keeping existing capture")
    shutil.rmtree(target, ignore_errors=True)
    tmp.rename(target)
    say(f"captured {count} snapshots")


def soak(minutes=5.0, runs=0, suite="varied", temperature=0.7, no_brain=False):
    from wlblib.cases import soak as soak_case

    return soak_case.run_soak(
        minutes=minutes,
        runs=runs,
        suite=suite,
        temperature=temperature,
        no_brain=no_brain,
    )


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
        for i, row0 in enumerate(runs[0]["rows"]):
            cells = []
            for r in runs:
                row = r["rows"][i]
                step = row["got"].get("plan_desc", "ERR")[:13]
                cells.append(f"{'PASS' if row['ok'] else 'FAIL'} ({step:11s})"[:18].ljust(18))
            say(f"  {row0['case']:25s} | " + " | ".join(cells))
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
