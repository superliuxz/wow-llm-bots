"""Shared helpers for tools/wlb: the repo's paths, running commands, and processes by pid."""

import dataclasses
import os
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
OUT = ROOT / "out"
VAR = ROOT / "var"
SERVER = OUT / "server"  # The installed game server (disposable: rebuilt by `wlb build server`).
RUN = SERVER / "run"  # Sockets, pid files and the console pipe of a running server.
ETC = SERVER / "etc"
LOGS = VAR / "logs"
MYSQL_CNF = ETC / "mysql.cnf"  # The one MySQL, shared by the play and test servers.


@dataclasses.dataclass(frozen=True)
class Instance:
    """One game server: the play server you log in to, or a test server e2e and perf run on
    (docs/design.md, E) while you play. Each has its own build and install, run directory (pid
    files, console pipe, brain socket), logs, databases and world port; they share MySQL, the
    client data and llama-server. Test shards (test_instance(k), k > 0: parallel e2e) share the
    one test build and install, and the test world database (tests don't write it); each has
    its own run-k and etc-k directories next to the install."""
    name: str
    root: pathlib.Path  # Build and install directory.
    logs: pathlib.Path
    db_prefix: str  # Database names: <prefix>auth, <prefix>characters, ...
    world_port: int
    test_hooks: bool  # The module's test-only code compiled in (src/test/).
    shard: int = 0  # A test shard's number; 0 for the play server and the plain test server.
    world_db: str = ""  # The world database, when it isn't <prefix>world (a test shard's).

    @property
    def run(self):
        return self.root / (f"run-{self.shard}" if self.shard else "run")

    @property
    def etc(self):
        """Its rendered config. A test shard's etc-k is bind-mounted over the install's etc for
        its worldserver (server.server_start): module configs are read from a path fixed when
        AzerothCore was compiled."""
        return self.root / (f"etc-{self.shard}" if self.shard else "etc")

    @property
    def install_etc(self):
        """The config templates (*.dist) the install put there."""
        return self.root / "etc"

    @property
    def bin(self):
        return self.root / "bin"

    def db(self, name):
        if name == "world" and self.world_db:
            return self.world_db
        return f"{self.db_prefix}{name}"


PLAY = Instance("play", SERVER, LOGS, "acore_", 8085, False)
TEST_ROOT = OUT / "server-test"
TEST_PORT = 8086
LLM_PORT = 8090  # Reserved for local llama-server (tools/wlb llm, eval, remote tunnel).
MAX_SHARDS = 8  # The most test servers at once: `wlb test e2e -j N`.


def test_instance(k):
    """Test server `k`: 0 is the plain test server (port 8086, acore_test_* databases,
    out/server-test/run, var/logs/test), what `wlb server --test`, `--keep-server` and perf use;
    shard k > 0 has port 8086 + k (skipping LLM_PORT 8090), acore_test_<k>_* databases (the world
    database shared), out/server-test/run-k and etc-k, and var/logs/test-k."""
    if not 0 <= k <= MAX_SHARDS:
        raise ValueError(f"no test shard {k} (0..{MAX_SHARDS})")
    if k == 0:
        return Instance("test", TEST_ROOT, LOGS / "test", "acore_test_", TEST_PORT, True)
    port = TEST_PORT + k
    if port >= LLM_PORT:
        port += 1
    return Instance(f"test-{k}", TEST_ROOT, LOGS / f"test-{k}", f"acore_test_{k}_",
                    port, True, shard=k, world_db="acore_test_world")


def _shard_from_env():
    """The test shard this process works on: $WLB_TEST_SHARD, set by `wlb test e2e -j N` for
    the process it starts per shard (0 when unset)."""
    text = os.environ.get("WLB_TEST_SHARD", "").strip()
    try:
        k = int(text) if text else 0
        test_instance(k)
    except ValueError:
        raise SystemExit(f"wlb: WLB_TEST_SHARD={text!r} is not a test shard (0..{MAX_SHARDS})")
    return k


# The test server this process works on (cases, Stack, soak): shard 0 unless a sharded e2e run
# started this process for another one.
TEST = test_instance(_shard_from_env())


class Failed(Exception):
    """A step failed; the message says which. `wlb` prints it and exits 1."""


def say(*parts):
    print(*parts, flush=True)


def run(cmd, *, cwd=None, check=True, quiet=False, env=None, **kwargs):
    """Runs `cmd` (a list), echoing nothing; raises Failed with the command if it fails."""
    cmd = [str(c) for c in cmd]
    result = subprocess.run(
        cmd, cwd=cwd, env={**os.environ, **(env or {})},
        stdout=subprocess.DEVNULL if quiet else None,
        stderr=subprocess.DEVNULL if quiet else None, **kwargs)
    if check and result.returncode != 0:
        raise Failed(f"failed ({result.returncode}): {shlex.join(cmd)}")
    return result


def output(cmd, **kwargs):
    """Runs `cmd` and returns its stdout as text (raises Failed if it fails)."""
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        raise Failed(f"failed ({result.returncode}): {shlex.join(map(str, cmd))}\n"
                     f"{result.stderr.strip()}")
    return result.stdout


def pid_file(name, inst=None):
    return (inst.run if inst else RUN) / f"{name}.pid"


def running_pid(name, inst=None):
    """The pid in <run>/<name>.pid (of `inst`, the play server by default) if that process is
    alive and really is `name` (a stale file whose pid was reused by something else doesn't
    count); otherwise None. Only this checkout's processes: never by name across the machine."""
    try:
        pid = int(pid_file(name, inst).read_text().strip())
        comm = pathlib.Path(f"/proc/{pid}/comm").read_text().strip()
    except (OSError, ValueError):
        return None
    return pid if comm == name else None


def alive(pid):
    """Whether `pid` is a running process. A zombie (exited, not yet reaped: a child this
    process started and hasn't waited for) is not: os.kill(pid, 0) still succeeds on one."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    # "pid (comm) S ...": the state follows the last ")" (comm may contain one).
    if stat[stat.rfind(")") + 2:][:1] in ("Z", "X"):
        try:
            os.waitpid(pid, os.WNOHANG)  # Reap it if it is ours.
        except ChildProcessError:
            pass
        return False
    return True


def need(path, how):
    """Fails with `how` (what builds it) if `path` doesn't exist."""
    if not pathlib.Path(path).exists():
        raise Failed(f"missing {pathlib.Path(path).relative_to(ROOT)}: {how}")


def main_guard(fn):
    """Runs fn(); a Failed is printed as one line and exits 1, Ctrl-C exits 130."""
    try:
        fn()
    except Failed as e:
        print(f"wlb: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
