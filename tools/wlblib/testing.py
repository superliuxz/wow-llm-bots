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

import importlib
import json
import pkgutil
import subprocess
import time

from wlblib import build, cases, server
from wlblib.common import LOGS, ROOT, Failed, running_pid, say, venv_python

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
    for target in TARGETS[1:]:
        found = discover(target)
        say(f"{target}: " + (", ".join(found) if found else "(no cases yet)"))
    say("unit: every gtest suite (wlb test unit [--filter PATTERN] [--variant asan|tsan|msan])")


def unit(variant="default", gtest_filter=None):
    build.common(variant, tests=True, gtest_filter=gtest_filter)
    say(f"PASS unit ({variant}{', filter ' + gtest_filter if gtest_filter else ''})")


# ---- e2e ----------------------------------------------------------------------------------------

class Stack:
    """A running test stack: the test build of the server with a test config, the test bot
    online, and a record of the server log from the start of the run.

    It is the same machine and database as the play server (an isolated stack with its own
    ports and a restored database is planned, docs/plans/phase2.md section E), so it stops the
    play server first and puts it back afterwards: same build, normal config, started again if it
    was running. Test characters are recreated from fixtures on every start; yours isn't touched."""

    def __init__(self, test_config, bot, keep=False):
        self.test_config, self.bot, self.keep = test_config, bot, keep
        self.was_running = False
        self.flavor_before = None
        self.log_start = 0

    def __enter__(self):
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
        python = venv_python()
        result = subprocess.run(
            [python, "-I", ROOT / "tools/snapshot/fake_harness.py", "--choose", "--json",
             f"--tail={tail_s}", str(seconds), *words], capture_output=True, text=True)
        if result.returncode != 0:
            raise Failed(f"the scripted brain failed:\n{result.stderr.strip()[-600:]}")
        return json.loads(result.stdout)


def e2e(names=(), keep_server=False):
    found = discover("e2e")
    unknown = [n for n in names if n not in found]
    if unknown:
        raise Failed(f"no e2e case {', '.join(unknown)} (have: {', '.join(found) or 'none'})")
    selected = [found[n] for n in names] if names else list(found.values())
    if not selected:
        raise Failed("no e2e cases")
    # Cases that share a test config share one server start.
    by_config = {}
    for module in selected:
        by_config.setdefault((module.TEST_CONFIG, module.BOT), []).append(module)
    results = []
    for (config, bot), modules in by_config.items():
        with Stack(config, bot, keep=keep_server) as stack:
            for module in modules:
                results.append(_run_case("e2e", module, stack))
    _summary(results)


# ---- perf and eval ------------------------------------------------------------------------------

def run_plain(target, names=()):
    """perf and eval cases manage what they need themselves (they get no stack)."""
    found = discover(target)
    unknown = [n for n in names if n not in found]
    if unknown:
        raise Failed(f"no {target} case {', '.join(unknown)} (have: {', '.join(found) or 'none'})")
    selected = [found[n] for n in names] if names else list(found.values())
    if not selected:
        raise Failed(f"no {target} cases yet")
    _summary([_run_case(target, module, None) for module in selected])


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
