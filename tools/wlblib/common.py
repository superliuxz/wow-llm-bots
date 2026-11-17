"""Shared helpers for tools/wlb: the repo's paths, running commands, and processes by pid."""

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
MYSQL_CNF = ETC / "mysql.cnf"
PROTOC = OUT / "protoc" / "protobuf" / "protoc"
VENV_PYTHON = OUT / "tools-venv" / "bin" / "python"


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


def pid_file(name):
    return RUN / f"{name}.pid"


def running_pid(name):
    """The pid in RUN/<name>.pid if that process is alive and really is `name` (a stale file
    whose pid was reused by something else doesn't count); otherwise None. Only this checkout's
    processes: never by name across the machine."""
    try:
        pid = int(pid_file(name).read_text().strip())
        comm = pathlib.Path(f"/proc/{pid}/comm").read_text().strip()
    except (OSError, ValueError):
        return None
    return pid if comm == name else None


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def need(path, how):
    """Fails with `how` (what builds it) if `path` doesn't exist."""
    if not pathlib.Path(path).exists():
        raise Failed(f"missing {pathlib.Path(path).relative_to(ROOT)}: {how}")


def venv_python():
    """The tools venv's Python (protobuf matching protoc), created by `wlb check snapshots`."""
    need(VENV_PYTHON, "run `tools/wlb check snapshots` once")
    return VENV_PYTHON


def main_guard(fn):
    """Runs fn(); a Failed is printed as one line and exits 1, Ctrl-C exits 130."""
    try:
        fn()
    except Failed as e:
        print(f"wlb: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
