"""`wlb remote`: sync, build, and run commands on optional remote CPU workers.

Remote workers are configured in `private/remotes.json` (untracked by the public-ready main repo;
override path with `WLB_REMOTES_FILE` for tests):

  {
    "worker1": {
      "ssh": "wlb@192.0.2.10",
      "repo": "~/wow-llm-bots",
      "llm_port": 8090
    }
  }

Guarantees:
  - Per-remote `flock` (`var/remote/<name>.lock`) serializes syncs, builds, and runs so a
    `post-commit` background sync and a `remote run` never collide.
  - Exact source stamp (`HEAD` + `git diff HEAD` + untracked non-ignored files) stored in remote
    `var/.built_stamp` only after `build common --no-tests` + `build server --test` succeed, so
    `remote run` can never execute an outdated binary.
  - Automatic SSH reverse tunnel (`-R 127.0.0.1:<port>:127.0.0.1:<port>`) whenever a local
    `llama-server` is listening on `<port>`, keeping `llama-server` bound to `127.0.0.1` with zero
    LAN exposure.
"""

from __future__ import annotations

import contextlib
import dataclasses
import fcntl
import hashlib
import json
import os
import pathlib
import re
import shlex
import socket
import subprocess
import sys

from wlblib.common import ROOT, VAR, Failed, output, run, say

DEFAULT_REMOTES_FILE = ROOT / "private" / "remotes.json"
DEFAULT_LLM_PORT = 8090
STAMP_REL = "var/.built_stamp"
HOOK_MARKER = "# wlb-remote-post-commit"

# Sync source tree cleanly with --delete while never touching remote build outputs, DBs, or links.
RSYNC_EXCLUDES = (
    "/out/",
    "/var/",
    "/models/",
    "/private/",
    "/.git/",
    "/third_party/azerothcore-wotlk/modules/mod-llm-brain",
    "/third_party/azerothcore-wotlk/modules/mod-playerbots",
    "/compile_commands.json",
)

_SAFE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


@dataclasses.dataclass(frozen=True)
class RemoteSpec:
    name: str
    ssh: str
    repo: str
    llm_port: int = DEFAULT_LLM_PORT


def remotes_config_path() -> pathlib.Path:
    override = os.environ.get("WLB_REMOTES_FILE")
    return pathlib.Path(override) if override else DEFAULT_REMOTES_FILE


def load_remotes(path: pathlib.Path | None = None) -> dict[str, RemoteSpec]:
    cfg = path or remotes_config_path()
    if not cfg.exists():
        return {}
    try:
        raw = json.loads(cfg.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise Failed(f"invalid remotes config {cfg}: {exc}") from exc
    if not isinstance(raw, dict):
        raise Failed(f"remotes config {cfg} must be a JSON object")
    out: dict[str, RemoteSpec] = {}
    for name, entry in raw.items():
        if not isinstance(name, str) or not _SAFE_NAME_RE.match(name):
            raise Failed(f"invalid remote name {name!r} in {cfg}")
        if not isinstance(entry, dict):
            raise Failed(f"remote {name!r} in {cfg} must be an object")
        ssh_target = entry.get("ssh")
        repo = entry.get("repo")
        llm_port = entry.get("llm_port", DEFAULT_LLM_PORT)
        if not isinstance(ssh_target, str) or not ssh_target.strip():
            raise Failed(f"remote {name!r} in {cfg} needs non-empty string 'ssh'")
        if not isinstance(repo, str) or not repo.strip():
            raise Failed(f"remote {name!r} in {cfg} needs non-empty string 'repo'")
        if not isinstance(llm_port, int) or isinstance(llm_port, bool) or not (1 <= llm_port <= 65535):
            raise Failed(f"remote {name!r} in {cfg} has invalid 'llm_port': {llm_port!r}")
        out[name] = RemoteSpec(name=name, ssh=ssh_target.strip(), repo=repo.strip().rstrip("/"),
                               llm_port=llm_port)
    return out


def get_remote(name: str, path: pathlib.Path | None = None) -> RemoteSpec:
    remotes = load_remotes(path)
    if name not in remotes:
        known = ", ".join(sorted(remotes)) or "(none configured)"
        raise Failed(f"unknown remote {name!r} (configured in {path or remotes_config_path()}: {known})")
    return remotes[name]


def _remote_cd_expr(repo: str) -> str:
    """Shell expression for `repo` that preserves leading `~/` expansion while safely quoting."""
    if repo == "~":
        return '"$HOME"'
    if repo.startswith("~/"):
        return f'"$HOME/"{shlex.quote(repo[2:])}'
    return shlex.quote(repo)


def _ssh_base_args(spec: RemoteSpec, *, tunnel_port: int | None = None) -> list[str]:
    args = [
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=5",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
    ]
    if tunnel_port is not None:
        args += [
            "-o", "ExitOnForwardFailure=yes",
            "-R", f"127.0.0.1:{tunnel_port}:127.0.0.1:{tunnel_port}",
        ]
    args.append(spec.ssh)
    return args


def _local_port_open(port: int, host: str = "127.0.0.1", timeout_s: float = 0.3) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout_s):
            return True
    except OSError:
        return False


def _excluded_from_stamp(rel: str) -> bool:
    parts = pathlib.PurePosixPath(rel).parts
    if not parts:
        return True
    if parts[0] in ("out", "var", "models", "private", ".git"):
        return True
    if rel == "compile_commands.json":
        return True
    return False


def compute_local_stamp(repo: pathlib.Path = ROOT) -> str:
    """Deterministic fingerprint of HEAD + tracked diff + untracked non-ignored source files."""
    head = output(["git", "-C", str(repo), "rev-parse", "HEAD"]).strip()
    h = hashlib.sha256()
    h.update(head.encode("utf-8"))
    h.update(b"\0diff\0")
    diff = subprocess.run(
        ["git", "-C", str(repo), "diff", "HEAD"],
        capture_output=True, check=False,
    )
    if diff.returncode != 0:
        raise Failed(f"git diff HEAD failed: {diff.stderr.decode('utf-8', 'replace').strip()}")
    h.update(diff.stdout)
    h.update(b"\0untracked\0")
    untracked_raw = output(
        ["git", "-C", str(repo), "ls-files", "--others", "--exclude-standard", "-z"]
    )
    for rel in sorted(p for p in untracked_raw.split("\0") if p):
        if _excluded_from_stamp(rel):
            continue
        fpath = repo / rel
        if not fpath.is_file() or fpath.is_symlink():
            continue
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        h.update(fpath.read_bytes())
        h.update(b"\0")
    return f"{head[:12]}:{h.hexdigest()[:16]}"


@contextlib.contextmanager
def _remote_lock(name: str):
    lock_dir = VAR / "remote"
    lock_dir.mkdir(parents=True, exist_ok=True)
    lock_path = lock_dir / f"{name}.lock"
    with lock_path.open("a+") as fp:
        try:
            fcntl.flock(fp.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            say(f"remote {name}: waiting for active sync/run lock ({lock_path})...")
            fcntl.flock(fp.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(fp.fileno(), fcntl.LOCK_UN)


def read_remote_stamp(spec: RemoteSpec) -> str | None:
    cmd = [
        *_ssh_base_args(spec),
        f"cd {_remote_cd_expr(spec.repo)} && cat {shlex.quote(STAMP_REL)} 2>/dev/null || true",
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise Failed(f"cannot reach remote {spec.name} ({spec.ssh}): {res.stderr.strip()}")
    stamp = res.stdout.strip()
    return stamp or None


def _ensure_remote_map_data(spec: RemoteSpec, repo: pathlib.Path = ROOT) -> None:
    """One-time push of static 3.3.5a client data (`out/server/data/`) if absent on the remote."""
    local_data = repo / "out" / "server" / "data"
    if not local_data.is_dir():
        raise Failed(f"local map data missing ({local_data}); run tools/wlb setup data first")
    probe = (
        f"cd {_remote_cd_expr(spec.repo)} 2>/dev/null && "
        "test -d out/server/data/dbc && test -d out/server/data/maps && "
        "test -d out/server/data/vmaps && test -d out/server/data/mmaps"
    )
    res = subprocess.run([*_ssh_base_args(spec), probe], capture_output=True, text=True)
    if res.returncode == 0:
        return
    say(f"remote {spec.name}: pushing static 3.3.5a map data (one-time) -> {spec.ssh}:{spec.repo}/out/server/data")
    run([*_ssh_base_args(spec), f"mkdir -p {_remote_cd_expr(spec.repo)}/out/server/data"])
    run(["rsync", "-a", "--info=progress2", f"{local_data}/", f"{spec.ssh}:{spec.repo}/out/server/data/"])


def _rsync_to_remote(spec: RemoteSpec, repo: pathlib.Path = ROOT) -> None:
    run([*_ssh_base_args(spec), f"mkdir -p {_remote_cd_expr(spec.repo)}"])
    cmd = ["rsync", "-a", "--delete"]
    for pat in RSYNC_EXCLUDES:
        cmd.append(f"--exclude={pat}")
    cmd += [f"{repo}/", f"{spec.ssh}:{spec.repo}/"]
    run(cmd)


def _stamp_satisfies(remote_stamp: str | None, local_stamp: str, *, server_build: bool) -> bool:
    if not remote_stamp:
        return False
    if remote_stamp == f"{local_stamp}:full":
        return True
    if not server_build and remote_stamp == f"{local_stamp}:common":
        return True
    return False


def _submodules_golden_key(repo: pathlib.Path = ROOT) -> str | None:
    third = repo / "third_party"
    try:
        return " ".join(
            output(["git", "-C", str(third / name), "rev-parse", "HEAD"]).strip()
            for name in ("azerothcore-wotlk", "mod-playerbots")
        )
    except Failed:
        return None


def _sync_unlocked(spec: RemoteSpec, *, force: bool = False, server_build: bool = True) -> str:
    local_stamp = compute_local_stamp()
    target_stamp = f"{local_stamp}:{'full' if server_build else 'common'}"
    if not force:
        remote_stamp = read_remote_stamp(spec)
        if _stamp_satisfies(remote_stamp, local_stamp, server_build=server_build):
            say(f"remote {spec.name}: up to date ({remote_stamp})")
            return remote_stamp

    say(f"remote {spec.name}: pushing source tree -> {spec.ssh}:{spec.repo}")
    _rsync_to_remote(spec)
    if server_build:
        _ensure_remote_map_data(spec)

    link_steps = [
        'ln -sfn "$PWD/third_party/mod-playerbots" "$PWD/third_party/azerothcore-wotlk/modules/mod-playerbots"',
        'ln -sfn "$PWD/server_module" "$PWD/third_party/azerothcore-wotlk/modules/mod-llm-brain"',
    ]
    build_steps = [
        *link_steps,
        "tools/wlb build common --no-tests",
    ]
    if server_build:
        build_steps.append("tools/wlb build server --test")
    sub_key = _submodules_golden_key()
    build_steps.append("mkdir -p var")
    if sub_key:
        build_steps.append(f"printf '%s\\n' {shlex.quote(sub_key)} > var/.golden_submodules")
    build_steps.append(f"printf '%s\\n' {shlex.quote(target_stamp)} > {shlex.quote(STAMP_REL)}")

    remote_script = f"set -euo pipefail; cd {_remote_cd_expr(spec.repo)}; " + "; ".join(build_steps)
    say(f"remote {spec.name}: building ({'common + server-test' if server_build else 'common'})...")
    run([*_ssh_base_args(spec), remote_script])
    say(f"remote {spec.name}: ready ({target_stamp})")
    return target_stamp


def list_remotes() -> None:
    remotes = load_remotes()
    if not remotes:
        say(f"no remotes configured ({remotes_config_path()})")
        return
    for name in sorted(remotes):
        spec = remotes[name]
        say(f"{name}\t{spec.ssh}\t{spec.repo}\tllm_port={spec.llm_port}")


def sync(name: str, *, force: bool = False, server_build: bool = True) -> str:
    spec = get_remote(name)
    with _remote_lock(spec.name):
        return _sync_unlocked(spec, force=force, server_build=server_build)


def status(name: str) -> None:
    spec = get_remote(name)
    local_stamp = compute_local_stamp()
    remote_stamp = read_remote_stamp(spec)
    state = "UP TO DATE" if _stamp_satisfies(remote_stamp, local_stamp, server_build=True) else "STALE"
    say(f"remote {spec.name} ({spec.ssh}:{spec.repo}): {state}")
    say(f"  local  stamp: {local_stamp}:full")
    say(f"  remote stamp: {remote_stamp or '(none)'}")
    probe = (
        f"cd {_remote_cd_expr(spec.repo)} && "
        "pgrep -a 'worldserver|mysqld|harness' 2>/dev/null || echo '  (no server/harness processes running)'"
    )
    res = subprocess.run([*_ssh_base_args(spec), probe], capture_output=True, text=True)
    if res.stdout.strip():
        say("  processes:")
        for line in res.stdout.strip().splitlines():
            say(f"    {line}")


def stop(name: str) -> None:
    spec = get_remote(name)
    with _remote_lock(spec.name):
        script = (
            f"cd {_remote_cd_expr(spec.repo)} && "
            "(tools/wlb server stop --test || true) && "
            "(tools/wlb db stop || true)"
        )
        run([*_ssh_base_args(spec), script])
        say(f"remote {spec.name}: stopped test server and db")


def pull(name: str, remote_rel: str, local_path: str | pathlib.Path) -> None:
    spec = get_remote(name)
    if pathlib.PurePosixPath(remote_rel).is_absolute() or ".." in pathlib.PurePosixPath(remote_rel).parts:
        raise Failed(f"remote path must be relative inside repo: {remote_rel!r}")
    dest = pathlib.Path(local_path)
    dest.parent.mkdir(parents=True, exist_ok=True)
    run(["rsync", "-a", f"{spec.ssh}:{spec.repo}/{remote_rel}", str(dest)])
    say(f"remote {spec.name}: pulled {remote_rel} -> {dest}")


def split_action_flags(tokens: list[str]) -> tuple[dict, list[str]]:
    """Parse leading wlb-remote flags that follow `<action>` (before `--` or the command)."""
    opts: dict = {
        "force": False,
        "no_sync": False,
        "no_tunnel": False,
        "no_server_build": False,
        "pull": [],
    }
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok == "--":
            i += 1
            break
        if tok == "--force":
            opts["force"] = True
            i += 1
        elif tok == "--no-sync":
            opts["no_sync"] = True
            i += 1
        elif tok == "--no-tunnel":
            opts["no_tunnel"] = True
            i += 1
        elif tok == "--no-server-build":
            opts["no_server_build"] = True
            i += 1
        elif tok == "--pull":
            if i + 1 >= len(tokens):
                raise Failed("--pull requires REMOTE_REL:LOCAL_PATH")
            opts["pull"].append(tokens[i + 1])
            i += 2
        elif tok.startswith("--pull="):
            opts["pull"].append(tok.split("=", 1)[1])
            i += 1
        else:
            break
    return opts, tokens[i:]


def _parse_pull_spec(spec_str: str) -> tuple[str, pathlib.Path]:
    if ":" not in spec_str:
        raise Failed(f"--pull must be REMOTE_REL:LOCAL_PATH, got {spec_str!r}")
    remote_rel, local_str = spec_str.split(":", 1)
    if not remote_rel or not local_str:
        raise Failed(f"--pull must be REMOTE_REL:LOCAL_PATH, got {spec_str!r}")
    return remote_rel, pathlib.Path(local_str)


def run_remote(
    name: str,
    cmd: list[str],
    *,
    no_sync: bool = False,
    no_tunnel: bool = False,
    server_build: bool = True,
    pulls: list[str] | tuple[str, ...] = (),
) -> None:
    if not cmd:
        raise Failed("remote run requires a command to execute")
    parsed_pulls = [_parse_pull_spec(p) for p in pulls]
    spec = get_remote(name)
    with _remote_lock(spec.name):
        if not no_sync:
            _sync_unlocked(spec, force=False, server_build=server_build)

        tunnel_port: int | None = None
        if not no_tunnel and _local_port_open(spec.llm_port):
            tunnel_port = spec.llm_port
            say(f"remote {spec.name}: reverse-tunneling local 127.0.0.1:{tunnel_port} -> remote")

        remote_cmd = f"cd {_remote_cd_expr(spec.repo)} && exec {shlex.join(cmd)}"
        ssh_cmd = [*_ssh_base_args(spec, tunnel_port=tunnel_port), remote_cmd]
        try:
            proc = subprocess.run(ssh_cmd)
        except KeyboardInterrupt:
            say(f"\nremote {spec.name}: interrupted; stopping remote test server...")
            cleanup = f"cd {_remote_cd_expr(spec.repo)} && (tools/wlb server stop --test || true)"
            subprocess.run([*_ssh_base_args(spec), cleanup], check=False)
            raise Failed("interrupted")

        for remote_rel, local_dest in parsed_pulls:
            try:
                pull(spec.name, remote_rel, local_dest)
            except Failed as exc:
                if proc.returncode == 0:
                    raise
                say(f"remote {spec.name}: warning: pull failed after command error: {exc}")

        if proc.returncode != 0:
            raise Failed(f"remote {spec.name} command failed ({proc.returncode}): {shlex.join(cmd)}")


def install_hook(name: str, repo: pathlib.Path = ROOT) -> pathlib.Path:
    """Install an untracked `.git/hooks/post-commit` that pre-warms `name` in the background."""
    spec = get_remote(name)
    hook_path = repo / ".git" / "hooks" / "post-commit"
    if hook_path.exists():
        existing = hook_path.read_text()
        if HOOK_MARKER not in existing:
            raise Failed(
                f"{hook_path} already exists and was not created by wlb remote; edit it manually"
            )
    log_path = VAR / "remote" / f"{spec.name}-sync.log"
    content = (
        "#!/usr/bin/env bash\n"
        f"{HOOK_MARKER}\n"
        '# Pre-warm remote worker binary after commits. Opt out with WLB_SKIP_REMOTE_SYNC=1.\n'
        '[ "${WLB_SKIP_REMOTE_SYNC:-0}" = "1" ] && exit 0\n'
        f"mkdir -p {shlex.quote(str(log_path.parent))}\n"
        f"nohup {shlex.quote(str(repo / 'tools' / 'wlb'))} remote {shlex.quote(spec.name)} sync "
        f"</dev/null >>{shlex.quote(str(log_path))} 2>&1 &\n"
    )
    hook_path.parent.mkdir(parents=True, exist_ok=True)
    hook_path.write_text(content)
    hook_path.chmod(0o755)
    say(f"installed post-commit auto-sync hook for remote {spec.name} -> {hook_path}")
    return hook_path
