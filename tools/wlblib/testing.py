"""`wlb test`: the test targets.

  unit   the gtest suites (common, module, harness); no game, no GPU. `ninja unit_tests`.
  e2e    live scenarios against a real worldserver (test build + a test config), pass or fail
  perf   what things cost, each checked against a bound, so a regression fails
  eval   how well the brain decides, on saved snapshots, scored
  all    e2e (parallel) and eval concurrently; fails if either fails

A case is a module in wlblib/cases/ named <target>_<name>.py with
  NAME         short name, as `wlb test e2e NAME` selects it
  DESCRIPTION  one line
  run(env)     does the case; raises (expect() fails with a message) when it fails
and, for e2e, TEST_CONFIG (config/tests/<TEST_CONFIG>.*.overrides) and BOT (the character to wait
for). The runner prints one PASS or FAIL line per case and exits 1 if any failed.

`wlb test e2e -j N` runs the e2e groups (cases sharing a TEST_CONFIG, BOT and ONLINE: one server
start each) on N test servers at once (common.test_instance), one `wlb test e2e` process per
shard, longest groups first by the last run's timings (var/e2e-timings.json).
"""

import datetime
import importlib
import json
import os
import pathlib
import pkgutil
import re
import select
import shutil
import signal
import socket
import subprocess
import threading
import time

from wlblib import build, cases, llm, remote as remote_mod, server, wire
from wlblib.common import (MAX_SHARDS, OUT, PLAY, ROOT, TEST, VAR, Failed, output, run,
                           running_pid, say, test_instance)

TARGETS = ("unit", "e2e", "live", "perf", "eval", "soak", "all")


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
    for target in ("e2e", "live", "perf"):
        found = discover(target)
        say(f"{target}: " + (", ".join(found) if found else "(no cases yet)"))
    from wlblib.cases import eval_plans
    say("eval: " + ", ".join(eval_plans.case_names()))
    say("unit: every gtest suite (wlb test unit [--filter PATTERN] [--variant asan|tsan|msan])")
    say("all: e2e (parallel) and eval concurrently (wlb test all [-j N] [--remote WORKER])")


def unit(variant="default", gtest_filter=None):
    build.common(variant, tests=True, gtest_filter=gtest_filter)
    say(f"PASS unit ({variant}{', filter ' + gtest_filter if gtest_filter else ''})")


# ---- e2e ----------------------------------------------------------------------------------------

class Stack:
    """The running test stack: an isolated test server (`inst`, by default this process's,
    common.TEST: its own port, run directory, logs and databases) with a test config and the
    test bot online, and a record of its log from the start of the run. Your play server keeps
    running meanwhile, untouched.

    Every run starts from the same state: the test server's databases are reset from their
    golden copy (server.test_databases_reset), and test characters are recreated from fixtures
    on every start."""

    def __init__(self, test_config, bot, keep=False, online=1, inst=None):
        self.test_config, self.bot, self.keep, self.online = test_config, bot, keep, online
        self.inst = inst or TEST
        self.socket = self.inst.run / "brain.sock"
        self.log_start = 0

    def __enter__(self):
        build.server(test=True)  # Only what changed since the last run.
        server.server_stop(inst=self.inst)  # One left running by --keep-server or a crash.
        try:
            server.test_databases_reset(self.inst)
            self._bring_up()
        except BaseException:
            server.server_stop(inst=self.inst)
            raise
        return self

    def _bring_up(self):
        server.config(self.test_config, inst=self.inst)
        server.server_start(self.inst)
        server.wait_online(self.bot, inst=self.inst)
        if self.online > 1:
            server.wait_online_count(self.online, inst=self.inst)
        try:
            self.log_start = (self.inst.logs / "Server.log").stat().st_size
        except OSError:
            self.log_start = 0
        # The first snapshots are sent as soon as a harness connects.

    def switch(self, test_config, bot, online=1):
        """Another test config on the same test server: a config and a restart."""
        self.test_config, self.bot, self.online = test_config, bot, online
        say(f"switching the test stack to {test_config}")
        try:
            server.server_stop(inst=self.inst)
            self._bring_up()
        except BaseException:
            if not self.keep:
                server.server_stop(inst=self.inst)
            raise

    def __exit__(self, *exc):
        if self.keep:
            say("leaving the test server running (--keep-server; stop it with "
                "`tools/wlb server stop --test`)")
        else:
            server.server_stop(inst=self.inst)
        return False

    def server_log(self):
        """The module's log lines since the stack came up."""
        try:
            with open(self.inst.logs / "Server.log", "rb") as f:
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


def _groups(selected):
    """Splits cases into (plain, groups): plain ones have no TEST_CONFIG; the others are grouped
    by (TEST_CONFIG, BOT, ONLINE), each group one server start, in the order first seen."""
    groups, plain = {}, []
    for module in selected:
        config = getattr(module, "TEST_CONFIG", None)
        if config:
            key = (config, module.BOT, getattr(module, "ONLINE", 1))
            groups.setdefault(key, []).append(module)
        else:
            plain.append(module)
    return plain, groups


def _group_name(key):
    config, bot, online = key
    return f"{config}/{bot}/{online}"


def _run_selected(selected, keep_server, call, record=None):
    """Runs `call(module, stack)` for every case. Cases with a TEST_CONFIG share one stack per
    (TEST_CONFIG, BOT, ONLINE); cases without one run with stack None, first (the play server
    stays up for them). With `record` (a dict), fills in record["cases"] (name, ok, seconds per
    case) and record["groups"] (seconds per group, its server start included) as it goes."""
    plain, groups = _groups(selected)
    results = []
    if record is not None:
        record.setdefault("cases", [])
        record.setdefault("groups", {})

    def one(module, stack):
        start = time.time()
        ok = call(module, stack)
        results.append(ok)
        if record is not None:
            record["cases"].append({"name": module.NAME, "ok": bool(ok),
                                    "seconds": round(time.time() - start, 1)})
        return ok

    for module in plain:
        one(module, None)
    # One test stack for the whole run: the test build is installed once and the play server
    # restored once; between configs only the config changes (a restart).
    if groups:
        keys = list(groups)
        config, bot, online = keys[0]
        started = time.time()
        with Stack(config, bot, keep=keep_server, online=online) as stack:
            for i, key in enumerate(keys):
                if i:
                    started = time.time()
                    stack.switch(*key)
                for module in groups[key]:
                    one(module, stack)
                if record is not None:
                    record["groups"][_group_name(key)] = round(time.time() - started, 1)
    _summary(results)


# ---- e2e, sharded -------------------------------------------------------------------------------

TIMINGS = VAR / "e2e-timings.json"  # Seconds per e2e group, from the last run that ran it.
RESULTS_ENV = "WLB_E2E_RESULTS"  # Where a shard's process writes its record (sharded runs).
# What one more test server costs: a test worldserver peaks at about 6 GB resident (its maps
# loaded, measured 2026-11-26), plus the harness and scripted brains its cases start.
SHARD_RAM_GB = 7.0
RAM_RESERVE_GB = 4.0  # Left free for everything else (the desktop, MySQL's growth).
GROUP_GUESS_S = 60.0  # A group never timed yet: a server start ...
CASE_GUESS_S = 60.0  # ... and this per case.
GROUP_BOOT_S = 45.0  # Extra server startup cost when splitting a multi-case group across shards.
SPLIT_MIN_GAIN_S = 15.0  # Only split a group if doing so cuts makespan by at least this much.


def _mem_available_gb():
    try:
        for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) / (1 << 20)
    except (OSError, ValueError, IndexError):
        pass
    return 0.0


def default_jobs(mem_available_gb=None, cpus=None):
    """How many test servers at once by default: one per SHARD_RAM_GB of memory available
    (after RAM_RESERVE_GB) and per two cores, at least 1, at most common.MAX_SHARDS."""
    mem = _mem_available_gb() if mem_available_gb is None else mem_available_gb
    cpus = (os.cpu_count() or 1) if cpus is None else cpus
    by_ram = int((mem - RAM_RESERVE_GB) // SHARD_RAM_GB)
    return max(1, min(MAX_SHARDS, by_ram, cpus // 2))


def _load_timings():
    try:
        data = json.loads(TIMINGS.read_text())
        return {k: float(v) for k, v in data.get("groups", {}).items()}
    except (OSError, ValueError, AttributeError, TypeError):
        return {}


def _load_case_timings():
    try:
        data = json.loads(TIMINGS.read_text())
        return {k: float(v) for k, v in data.get("cases", {}).items()}
    except (OSError, ValueError, AttributeError, TypeError):
        return {}


def _save_timings(groups, cases=None):
    """Merges this run's group and per-case times into TIMINGS (entries not run keep their last
    time)."""
    if not groups and not cases:
        return
    merged_groups = _load_timings()
    if groups:
        merged_groups.update(groups)
    merged_cases = _load_case_timings()
    if cases:
        merged_cases.update(cases)
    payload = {"groups": dict(sorted(merged_groups.items()))}
    if merged_cases:
        payload["cases"] = dict(sorted(merged_cases.items()))
    TIMINGS.parent.mkdir(parents=True, exist_ok=True)
    tmp = TIMINGS.with_name(TIMINGS.name + ".tmp")
    tmp.write_text(json.dumps(payload, indent=1) + "\n")
    tmp.rename(TIMINGS)


def plan_shards(groups, jobs, timings):
    """Puts groups ({key: [case names]}) on `jobs` shards, longest first, each on the shard with
    the least work so far (by `timings`, {group name: seconds}; a group not timed yet is guessed
    from its case count). Returns one list of keys per shard (no empty shards), in the order the
    shard runs them, and each shard's estimated seconds."""
    def estimate(key):
        return timings.get(_group_name(key), GROUP_GUESS_S + CASE_GUESS_S * len(groups[key]))

    order = sorted(groups, key=lambda k: (-estimate(k), _group_name(k)))
    shards = [[] for _ in range(max(1, min(jobs, len(groups))))]
    load = [0.0] * len(shards)
    for key in order:
        i = min(range(len(shards)), key=lambda j: (load[j], j))
        shards[i].append(key)
        load[i] += estimate(key)
    return shards, load


def _case_estimates(key, names, timings, case_timings):
    """Estimated seconds per case in `names` for `key`, using recorded per-case timings when
    available and distributing any remaining group time evenly across untimed cases."""
    gname = _group_name(key)
    known = {n: case_timings[n] for n in names if n in case_timings}
    unknown = [n for n in names if n not in known]
    if not unknown:
        return known
    if gname in timings:
        rem = max(float(len(unknown)), timings[gname] - GROUP_BOOT_S - sum(known.values()))
        each = rem / len(unknown)
    else:
        each = CASE_GUESS_S
    return {n: known.get(n, each) for n in names}


def _pack_chunks(chunks, jobs):
    """LPT-packs `(key, case_names, est_s)` chunks onto at most `jobs` shards."""
    order = sorted(chunks, key=lambda c: (-c[2], _group_name(c[0]), c[1]))
    shards = [[] for _ in range(max(1, min(jobs, len(order))))]
    load = [0.0] * len(shards)
    for item in order:
        i = min(range(len(shards)), key=lambda j: (load[j], j))
        shards[i].append(item)
        load[i] += item[2]
    return shards, load


def _group_chunks(key, names, count, timings, case_timings):
    """Partitions `names` of group `key` into `count` balanced `(key, chunk_names, est_s)` chunks."""
    if count <= 1 or len(names) <= 1:
        gname = _group_name(key)
        if gname in timings:
            est = timings[gname]
        elif names and all(n in case_timings for n in names):
            est = GROUP_BOOT_S + sum(case_timings[n] for n in names)
        else:
            est = GROUP_GUESS_S + sum(case_timings.get(n, CASE_GUESS_S) for n in names)
        return [(key, list(names), est)]
    c_est = _case_estimates(key, names, timings, case_timings)
    k = min(count, len(names))
    parts = [[] for _ in range(k)]
    pload = [0.0] * k
    for name in sorted(names, key=lambda n: (-c_est[n], n)):
        j = min(range(k), key=lambda idx: (pload[idx], idx))
        parts[j].append(name)
        pload[j] += c_est[name]
    return [(key, part, GROUP_BOOT_S + load_s) for part, load_s in zip(parts, pload) if part]


def plan_shard_cases(groups, jobs, timings, case_timings=None):
    """Packs `groups` ({key: [case names]}) onto at most `jobs` shards, automatically splitting
    oversized multi-case groups into 2+ balanced chunks whenever doing so reduces the estimated
    makespan by at least `SPLIT_MIN_GAIN_S` after paying `GROUP_BOOT_S` per extra server start.
    Re-partitions a split group as a whole (`k -> k+1` balanced parts) rather than bisecting a
    single existing chunk, avoiding local minima when two halves tie at the makespan.
    Returns `(shards, load)` where each shard is a list of `(key, [case_names])` chunks."""
    case_timings = case_timings or {}
    split_counts = {key: 1 for key in groups}

    def build_chunks(counts):
        out = []
        for key, names in groups.items():
            out.extend(_group_chunks(key, names, counts[key], timings, case_timings))
        return out

    chunks = build_chunks(split_counts)
    if jobs > 1:
        while True:
            _, load = _pack_chunks(chunks, jobs)
            cur_max = max(load)
            best = None  # (new_max, total_load, group_name, key, cand_chunks)
            for key, names in groups.items():
                if split_counts[key] >= len(names):
                    continue
                cand_counts = dict(split_counts)
                cand_counts[key] += 1
                cand_chunks = build_chunks(cand_counts)
                _, new_load = _pack_chunks(cand_chunks, jobs)
                new_max = max(new_load)
                if new_max < cur_max - SPLIT_MIN_GAIN_S:
                    score = (new_max, sum(new_load), _group_name(key))
                    if best is None or score < best[:3]:
                        best = (*score, key, cand_chunks)
            if best is None:
                break
            split_counts[best[3]] += 1
            chunks = best[4]

    packed, load = _pack_chunks(chunks, jobs)
    return [[(k, ns) for k, ns, _ in shard] for shard in packed], load


# A case's result line; console output just before it may share its line ("AC> PASS e2e ...").
_RESULT_LINE = re.compile(r"(?:^|\s)(?:PASS|FAIL) e2e \S")


class _CaseBlocks:
    """Collects one shard's output lines and prints each case's lines whole, ending at its
    PASS or FAIL line, so the shards' output doesn't interleave. Lines other than the result
    lines carry the shard's name."""

    def __init__(self, label, lock, emit=None):
        self.label, self.lock, self.lines = label, lock, []
        self.emit = emit or (lambda text: print(text, flush=True))

    def feed(self, line):
        line = line.rstrip("\n")
        if _RESULT_LINE.search(line):
            self.lines.append(line)
            self.flush()
        else:
            self.lines.append(f"  [{self.label}] {line}")

    def flush(self):
        if self.lines:
            with self.lock:
                self.emit("\n".join(self.lines))
            self.lines = []


def _run_sharded(groups, jobs, *, shards=None, load=None):
    """Runs the groups on `jobs` test servers at once, one `wlb test e2e` process per shard
    (shards 0..jobs-1), and prints the same summary as a serial run."""
    started = time.time()
    build.server(test=True)  # Once, for all shards: theirs skip building (build.PREBUILT_ENV).
    server.test_servers_stop()  # Left running by --keep-server or a crash.
    server.golden_databases_make()  # Once, before the shards copy it.
    if shards is None or load is None:
        shards, load = plan_shard_cases({k: [m.NAME for m in v] for k, v in groups.items()}, jobs,
                                        _load_timings(), _load_case_timings())
    chunk_counts = {}
    for shard in shards:
        for key, _ in shard:
            chunk_counts[key] = chunk_counts.get(key, 0) + 1
    split_group_names = {_group_name(k) for k, cnt in chunk_counts.items() if cnt > 1}
    total_cases = sum(len(ns) for shard in shards for _, ns in shard)
    say(f"running {total_cases} e2e cases in {len(groups)} groups on "
        f"{len(shards)} test servers (estimated {max(load) / 60:.1f} min)")
    lock = threading.Lock()
    procs = []
    try:
        for k, shard in enumerate(shards):
            names = [name for _, chunk_names in shard for name in chunk_names]
            results = OUT / "e2e-shards" / f"shard-{k}.json"
            results.parent.mkdir(parents=True, exist_ok=True)
            results.unlink(missing_ok=True)
            env = {**os.environ, "WLB_TEST_SHARD": str(k), build.PREBUILT_ENV: "1",
                   RESULTS_ENV: str(results), "PYTHONUNBUFFERED": "1"}
            proc = subprocess.Popen([ROOT / "tools" / "wlb", "test", "e2e", "-j", "1", *names],
                                    cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    errors="replace")
            blocks = _CaseBlocks(test_instance(k).name, lock)
            reader = threading.Thread(target=_pump, args=(proc, blocks), daemon=True)
            reader.start()
            procs.append((k, names, results, proc, reader))
        for _, _, _, proc, reader in procs:
            proc.wait()
            reader.join()
    except BaseException as e:
        # A shard stops its own server on SIGINT, as a serial run does on Ctrl-C. Ctrl-C has
        # reached the shards already (one process group): a second one would cut their cleanup.
        for _, _, _, proc, _ in procs:
            if proc.poll() is None and not isinstance(e, KeyboardInterrupt):
                proc.send_signal(signal.SIGINT)
        for _, _, _, proc, _ in procs:
            try:
                proc.wait(150)
            except subprocess.TimeoutExpired:
                proc.kill()
        raise
    finally:
        server.test_servers_stop()  # Whatever a shard left behind (normally nothing).
    oks, timed, case_timed = [], {}, {}
    for k, names, results, proc, _ in procs:
        try:
            record = json.loads(results.read_text())
        except (OSError, ValueError):
            record = {}
        ran = {}
        for c in record.get("cases", []):
            ran[c["name"]] = c["ok"]
            if "seconds" in c:
                case_timed[c["name"]] = float(c["seconds"])
        for name in names:
            if name not in ran:
                say(f"FAIL e2e {name}: not run (the {test_instance(k).name} process exited "
                    f"{proc.returncode} first)")
            oks.append(bool(ran.get(name, False)))
        for gname, secs in record.get("groups", {}).items():
            # A group split across multiple shards only reports a partial chunk time per shard;
            # keep its whole-group timing intact (per-case timings handle future split planning).
            if gname not in split_group_names:
                timed[gname] = secs
    _save_timings(timed, case_timed)
    say(f"e2e on {len(shards)} test servers: {(time.time() - started) / 60:.1f} min")
    _summary(oks)


def _pump(proc, blocks):
    for line in proc.stdout:
        blocks.feed(line)
    blocks.flush()


def split_groups_for_remote(groups, timings, local_jobs=1, remote_jobs=1):
    """Splits e2e `groups` ({key: [case names]}) across local (`local_jobs` shards) and remote
    (`remote_jobs` shards) by packing into `local_jobs + remote_jobs` balanced bins with
    `plan_shards` and assigning proportional bins to each side (heaviest bins to local). With
    <= 1 group or non-positive `remote_jobs`, puts everything on local and leaves remote empty."""
    if len(groups) <= 1 or remote_jobs <= 0:
        return list(groups), []
    local_jobs = max(1, local_jobs)
    total = min(len(groups), local_jobs + remote_jobs)
    r_bins = max(1, min(total - 1, round(total * remote_jobs / (local_jobs + remote_jobs))))
    l_bins = total - r_bins
    shards, _ = plan_shards(groups, total, timings)
    local_keys = [k for s in shards[:l_bins] for k in s]
    remote_keys = [k for s in shards[l_bins:] for k in s]
    return local_keys, remote_keys


def split_cases_for_remote(groups, timings, case_timings=None, local_jobs=1, remote_jobs=1):
    """Splits e2e `groups` ({key: [case names]}) across local (`local_jobs` shards) and remote
    (`remote_jobs` shards) using `plan_shard_cases` (which splits oversized multi-case groups when
    it cuts makespan). Returns `(local_chunks, remote_chunks)` of `(key, [case_names])`."""
    if remote_jobs <= 0 or (len(groups) <= 1 and sum(len(v) for v in groups.values()) <= 1):
        return [(k, list(v)) for k, v in groups.items()], []
    local_jobs = max(1, local_jobs)
    shards, _ = plan_shard_cases(groups, local_jobs + remote_jobs, timings, case_timings)
    if len(shards) <= 1:
        return [c for s in shards for c in s], []
    total = len(shards)
    r_bins = max(1, min(total - 1, round(total * remote_jobs / (local_jobs + remote_jobs))))
    l_bins = total - r_bins
    local_chunks = [c for s in shards[:l_bins] for c in s]
    remote_chunks = [c for s in shards[l_bins:] for c in s]
    return local_chunks, remote_chunks


def _remote_default_jobs(remote_name):
    """Queries `default_jobs()` on `remote_name` (already synced); falls back to 1 on error."""
    wlb = ROOT / "tools" / "wlb"
    probe = "import sys; sys.path.insert(0, 'tools'); from wlblib import testing; print(testing.default_jobs())"
    try:
        res = subprocess.run(
            [wlb, "remote", remote_name, "run", "--no-sync", "--", "python3", "-c", probe],
            cwd=ROOT, capture_output=True, text=True, timeout=30,
        )
        if res.returncode == 0:
            for line in reversed(res.stdout.splitlines()):
                line = line.strip()
                if line.isdigit():
                    return max(1, min(MAX_SHARDS, int(line)))
    except (OSError, subprocess.SubprocessError):
        pass
    return 1


def _remote_e2e_commands(local_names, remote_names, remote_name, *, jobs=None, keep_server=False):
    """Builds the `(label, argv, env_extra)` tuples for split local+remote e2e."""
    wlb = ROOT / "tools" / "wlb"
    local_cmd = [wlb, "test", "e2e"]
    if jobs is not None:
        local_cmd += ["-j", str(jobs)]
    if keep_server:
        local_cmd.append("--keep-server")
    local_cmd += list(local_names)

    remote_cmd = [
        wlb, "remote", remote_name, "run", "--no-sync", "--",
        "env", f"{build.PREBUILT_ENV}=1", "tools/wlb", "test", "e2e", *remote_names,
    ]
    return [
        ("local", local_cmd, {build.PREBUILT_ENV: "1", "PYTHONUNBUFFERED": "1"}),
        (f"remote:{remote_name}", remote_cmd, {"PYTHONUNBUFFERED": "1"}),
    ]


def _run_e2e_remote(groups, jobs, keep_server, remote_name):
    """Syncs `remote_name`, splits `groups` across local and `remote_name` proportional to each
    machine's shard capacity, runs both halves concurrently, streams prefixed output, merges
    results, and tears down cleanly on failure or interrupt. Returns True if split ran, False if
    <= 1 group (caller runs locally)."""
    remote_mod.get_remote(remote_name)  # Validate worker name before building/syncing.
    if len(groups) <= 1:
        say(f"only {len(groups)} e2e group; running locally without remote {remote_name}")
        return False

    started = time.time()
    build.common("default", tests=False)
    build.server(test=True)
    remote_mod.sync(remote_name, server_build=True)

    local_jobs = default_jobs() if jobs is None else jobs
    remote_jobs = _remote_default_jobs(remote_name)
    group_names = {k: [m.NAME for m in v] for k, v in groups.items()}
    local_chunks, remote_chunks = split_cases_for_remote(
        group_names, _load_timings(), _load_case_timings(),
        local_jobs=local_jobs, remote_jobs=remote_jobs,
    )
    if not remote_chunks:
        say(f"only {len(groups)} e2e group; running locally without remote {remote_name}")
        return False

    local_names = [name for _, ns in local_chunks for name in ns]
    remote_names = [name for _, ns in remote_chunks for name in ns]
    local_group_count = len({k for k, _ in local_chunks})
    remote_group_count = len({k for k, _ in remote_chunks})
    cmds = _remote_e2e_commands(local_names, remote_names, remote_name, jobs=jobs,
                                keep_server=keep_server)
    say(f"splitting {len(local_names) + len(remote_names)} e2e cases across local "
        f"({len(local_names)} cases, {local_group_count} groups, -j {local_jobs}) and remote "
        f"{remote_name} ({len(remote_names)} cases, {remote_group_count} groups, -j {remote_jobs})")
    lock = threading.Lock()
    procs = []
    try:
        for label, cmd, env_extra in cmds:
            env = {**os.environ, **env_extra}
            proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    errors="replace")
            reader = threading.Thread(target=_stream_lines, args=(proc, label, lock), daemon=True)
            reader.start()
            procs.append((label, proc, reader))
        results = {}
        for label, proc, reader in procs:
            rc = proc.wait()
            reader.join()
            results[label] = rc
    except BaseException as e:
        for _, proc, _ in procs:
            if proc.poll() is None and not isinstance(e, KeyboardInterrupt):
                proc.send_signal(signal.SIGINT)
        for _, proc, reader in procs:
            try:
                proc.wait(150)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            reader.join(timeout=5)
        raise
    finally:
        if not keep_server:
            server.test_servers_stop()

    elapsed_min = (time.time() - started) / 60
    summary = ", ".join(f"{label}={'PASS' if rc == 0 else f'FAIL ({rc})'}"
                        for label, rc in results.items())
    say(f"e2e split ({elapsed_min:.1f} min): {summary}")
    failed = [f"{label} (exit {rc})" for label, rc in results.items() if rc != 0]
    if failed:
        raise Failed("e2e split failed: " + ", ".join(failed))
    return True


def e2e(names=(), keep_server=False, jobs=None, remote=None):
    """The e2e cases, on one test server (`jobs` 1: in this process), on `jobs` at once
    (default: default_jobs(); never more than there are groups), or split across this machine
    and `remote` (`--remote WORKER`)."""
    selected = _select("e2e", names)
    missing = [m.NAME for m in selected if not getattr(m, "TEST_CONFIG", None)]
    if missing:
        raise Failed(f"e2e cases need a TEST_CONFIG: {', '.join(missing)}")
    _, groups = _groups(selected)
    if remote and _run_e2e_remote(groups, jobs, keep_server, remote):
        return
    build.common("default", tests=False)
    asked = jobs
    jobs = min(default_jobs() if jobs is None else jobs, len(selected))
    if jobs > 1 and keep_server:
        if asked:
            say("--keep-server: running on one test server")
        jobs = 1
    if jobs > 1 and not server.own_etc_supported():
        say("this machine doesn't allow unprivileged mount namespaces (unshare --user "
            "--mount): running on one test server")
        jobs = 1
    if jobs > 1:
        group_names = {k: [m.NAME for m in v] for k, v in groups.items()}
        shards, load = plan_shard_cases(group_names, jobs, _load_timings(), _load_case_timings())
        if len(shards) > 1:
            _run_sharded(groups, jobs, shards=shards, load=load)
            return
    record = {}
    results = os.environ.get(RESULTS_ENV)
    try:
        _run_selected(selected, keep_server, lambda module, stack: _run_case("e2e", module, stack),
                      record=record)
    finally:
        if results:  # A shard of a sharded run: its parent merges this and saves the times.
            pathlib.Path(results).write_text(json.dumps(record))
        else:
            case_timed = {c["name"]: float(c["seconds"]) for c in record.get("cases", [])
                          if "seconds" in c}
            _save_timings(record.get("groups", {}), case_timed)


# ---- live (live-model E2E) ----------------------------------------------------------------------

LIVE_TEMPERATURE_ENV = "WLB_LIVE_TEMPERATURE"


def live(names=(), keep_server=False, model=None, temperature=0.0):
    """Live-model E2E scenarios against a real isolated test server + real llama-server (:8090).

    Kept separate from `wlb test e2e` so mock-LLM e2e stays CPU-only, deterministic, shardable
    with `--remote`, and non-conflicting with `eval`'s llama-server during `wlb test all`.
    """
    selected = _select("live", names)
    missing = [m.NAME for m in selected if not getattr(m, "TEST_CONFIG", None)]
    if missing:
        raise Failed(f"live cases need a TEST_CONFIG: {', '.join(missing)}")
    build.common("default", tests=False)
    prev_temp = os.environ.get(LIVE_TEMPERATURE_ENV)
    os.environ[LIVE_TEMPERATURE_ENV] = str(temperature)
    try:
        with LlamaServer(slots=8, model=model):
            _run_selected(selected, keep_server,
                          lambda module, stack: _run_case("live", module, stack))
    finally:
        if prev_temp is None:
            os.environ.pop(LIVE_TEMPERATURE_ENV, None)
        else:
            os.environ[LIVE_TEMPERATURE_ENV] = prev_temp


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
        label = self.model or "the default model, wlb llm"
        say(f"starting llama-server ({label})")
        (OUT / "logs").mkdir(parents=True, exist_ok=True)
        cmd = [ROOT / "tools" / "wlb", "llm", "--slots", str(self.slots)]
        if self.model:
            cmd += ["--model", self.model]
        with open(OUT / "logs" / "llama-perf.log", "wb") as log:
            self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + self.timeout_s
            while time.time() < deadline:
                if self.proc.poll() is not None:
                    raise Failed("llama-server exited (see out/logs/llama-perf.log)")
                if self.healthy():
                    return self
                time.sleep(2)
            raise Failed(f"llama-server not healthy after {self.timeout_s} s")
        except BaseException:
            self.__exit__()
            raise

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
    out/bench/snapshots/quest, the input of the brain load replay (`perf brain`, tools/wlblib/cases/perf_brain.py).
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


def soak(
    minutes=5.0,
    runs=0,
    suite="varied",
    temperature=0.7,
    no_brain=False,
    max_chatter_per_hour=None,
    chatter_llm_judge=False,
):
    from wlblib.cases import soak as soak_case

    kwargs = {
        "minutes": minutes,
        "runs": runs,
        "suite": suite,
        "temperature": temperature,
        "no_brain": no_brain,
        "chatter_llm_judge": chatter_llm_judge,
    }
    if max_chatter_per_hour is not None:
        kwargs["max_chatter_per_hour"] = max_chatter_per_hour
    return soak_case.run_soak(**kwargs)


def eval_(names=(), model=None, shootout=False, live=False, samples=None):
    from wlblib.cases import eval_plans

    cases = eval_plans.load_cases(names, live=live)
    if live:
        with LlamaServer(slots=4, model=model):
            res = eval_plans.run_live(cases, samples=samples or eval_plans.LIVE_SAMPLES)
        if res["draws"] and res["mean_rate"] < eval_plans.LIVE_MIN_PASS_RATE:
            raise Failed(f"live eval passed {100 * res['mean_rate']:.0f}% of draws "
                         f"(< {100 * eval_plans.LIVE_MIN_PASS_RATE:.0f}%)")
        return
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


# ---- all (e2e + eval concurrently) --------------------------------------------------------------

def _all_commands(jobs=None, keep_server=False, model=None, live=False, samples=None,
                  remote=None):
    """Builds the (label, argv) pairs for `wlb test all`."""
    wlb = ROOT / "tools" / "wlb"
    e2e_cmd = [wlb, "test", "e2e"]
    if jobs is not None:
        e2e_cmd += ["-j", str(jobs)]
    if keep_server:
        e2e_cmd.append("--keep-server")
    if remote:
        e2e_cmd += ["--remote", remote]

    eval_cmd = [wlb, "test", "eval"]
    if model:
        eval_cmd += ["--model", model]
    if live:
        eval_cmd.append("--live")
    if samples is not None:
        eval_cmd += ["--samples", str(samples)]
    return [("e2e", e2e_cmd), ("eval", eval_cmd)]


def _stream_lines(proc, label, lock, emit=None):
    if proc.stdout is None:
        return
    emit = emit or (lambda text: print(text, flush=True))
    for line in proc.stdout:
        with lock:
            emit(f"[{label}] {line.rstrip('\r\n')}")


def all_(jobs=None, keep_server=False, model=None, live=False, samples=None, remote=None):
    """Runs `e2e` (parallel by default, `-j N`, or split with `--remote WORKER`) concurrently
    with `eval` (GPU/llama-server). Streams both outputs prefixed by suite name, reports both
    results, and fails if either fails."""
    if remote:
        remote_mod.get_remote(remote)  # Validate worker name before building.
    started = time.time()
    build.common("default", tests=False)
    build.server(test=True)
    cmds = _all_commands(jobs=jobs, keep_server=keep_server, model=model, live=live,
                         samples=samples, remote=remote)
    env = {**os.environ, build.PREBUILT_ENV: "1", "PYTHONUNBUFFERED": "1"}
    lock = threading.Lock()
    procs = []
    say("running e2e and eval concurrently")
    try:
        for label, cmd in cmds:
            proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    errors="replace")
            reader = threading.Thread(target=_stream_lines, args=(proc, label, lock), daemon=True)
            reader.start()
            procs.append((label, proc, reader))
        results = {}
        for label, proc, reader in procs:
            rc = proc.wait()
            reader.join()
            results[label] = rc
    except BaseException as e:
        for _, proc, _ in procs:
            if proc.poll() is None and not isinstance(e, KeyboardInterrupt):
                proc.send_signal(signal.SIGINT)
        for _, proc, reader in procs:
            try:
                proc.wait(150)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            reader.join(timeout=5)
        raise
    finally:
        if not keep_server:
            server.test_servers_stop()

    elapsed_min = (time.time() - started) / 60
    summary = ", ".join(f"{label}={'PASS' if rc == 0 else f'FAIL ({rc})'}"
                        for label, rc in results.items())
    say(f"test all ({elapsed_min:.1f} min): {summary}")
    failed = [f"{label} (exit {rc})" for label, rc in results.items() if rc != 0]
    if failed:
        raise Failed("test all failed: " + ", ".join(failed))


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
