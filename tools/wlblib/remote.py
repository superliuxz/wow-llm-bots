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
  - Exact source stamp (`HEAD` + `git diff HEAD` + untracked non-ignored files) is invalidated on
    the remote *before* pushing and written to `var/.built_stamp` only after `build common
    --no-tests` (+ `build server --test`) succeeds, so `remote run` can never execute an outdated
    or half-built binary.
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
import secrets
import shlex
import socket
import subprocess

from wlblib.common import ROOT, VAR, Failed, output, run, say

DEFAULT_REMOTES_FILE = ROOT / "private" / "remotes.json"
DEFAULT_LLM_PORT = 8090
STAMP_REL = "var/.built_stamp"
MAP_DATA_SENTINEL_REL = "out/server/data/.synced"
ACTIVE_PGID_REL = "var/.active_remote_pgid"
REMOTE_LOCK_REL = "var/.remote.lock"
REPO_SENTINEL_REL = ".wlb-remote-repo"
HOOK_MARKER = "# wlb-remote-post-commit"

# Sync source tree cleanly with --delete while never touching remote build outputs, DBs, links,
# or untracked local caches/reference dumps. `.git` (unanchored) excludes both root `/.git/` and
# submodule gitdir pointer files (`third_party/*/.git`).
RSYNC_EXCLUDES = (
    "/out/",
    "/var/",
    "/models/",
    "/private/",
    "/reference/",
    "/.cache/",
    "/.wlb-remote-repo",
    ".git",
    "__pycache__/",
    "*.pyc",
    "/third_party/azerothcore-wotlk/modules/mod-llm-brain",
    "/third_party/azerothcore-wotlk/modules/mod-playerbots",
    "/compile_commands.json",
)

_SAFE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_SSH_COMMON_OPTS = (
    "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=5",
    "-o", "ServerAliveInterval=30",
    "-o", "ServerAliveCountMax=3",
)
_VALID_ACTIONS = ("sync", "status", "run", "pull", "stop", "install-hook")


@dataclasses.dataclass(frozen=True)
class RemoteSpec:
    name: str
    ssh: str
    repo: str
    llm_port: int = DEFAULT_LLM_PORT


def remotes_config_path() -> pathlib.Path:
    override = os.environ.get("WLB_REMOTES_FILE")
    return pathlib.Path(override) if override else DEFAULT_REMOTES_FILE


_FORBIDDEN_REPO_ROOTS = frozenset({
    "/", "/home", "/root", "/tmp", "/var", "/etc", "/usr", "/opt", "/srv", "/mnt", "/media",
})
_UNSAFE_REPO_CHARS_RE = re.compile(r"[\s;&|`$\n\r<>()*?\[\]'\"\\#{}!]")


def _validate_remote_repo(repo: str) -> str:
    """Refuse unsafe remote repo paths (`~`, `/`, `$HOME`, `.`, `..`, shell metachars, top-level dirs)."""
    cleaned = repo.strip().rstrip("/")
    if not cleaned:
        raise Failed("remote 'repo' must be a non-empty subdirectory path")
    if _UNSAFE_REPO_CHARS_RE.search(cleaned):
        raise Failed(f"refusing unsafe remote 'repo' {repo!r}: contains shell metacharacters")
    if cleaned in ("~", ".", ".."):
        raise Failed(f"refusing unsafe remote 'repo' {repo!r}: must be a dedicated subdirectory below $HOME")
    if cleaned.startswith("~") and not cleaned.startswith("~/"):
        raise Failed(f"refusing unsafe remote 'repo' {repo!r}: use '~/subdir' or an absolute subdirectory path")
    sub = cleaned[2:] if cleaned.startswith("~/") else cleaned
    # Check raw `/`-split segments BEFORE PurePosixPath normalization so `/.` and `/home/.` are caught.
    raw_segments = [seg for seg in sub.split("/") if seg != ""]
    if not raw_segments or any(seg in (".", "..") for seg in raw_segments):
        raise Failed(f"refusing unsafe remote 'repo' {repo!r}: must not contain '.' or '..' components")
    if cleaned in _FORBIDDEN_REPO_ROOTS:
        raise Failed(f"refusing unsafe remote 'repo' {repo!r}: must be a dedicated subdirectory")
    return cleaned


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
        validated_repo = _validate_remote_repo(repo)
        out[name] = RemoteSpec(name=name, ssh=ssh_target.strip(), repo=validated_repo,
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
    args = ["ssh", *_SSH_COMMON_OPTS]
    if tunnel_port is not None:
        args += [
            "-o", "ExitOnForwardFailure=yes",
            "-R", f"127.0.0.1:{tunnel_port}:127.0.0.1:{tunnel_port}",
        ]
    args.append(spec.ssh)
    return args


def _rsync_ssh_cmd() -> str:
    return shlex.join(["ssh", *_SSH_COMMON_OPTS])


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
    if parts[0] in ("out", "var", "models", "private", "reference", ".cache", ".git"):
        return True
    if "__pycache__" in parts or rel.endswith(".pyc") or rel == "compile_commands.json":
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
        f"cd {_remote_cd_expr(spec.repo)} 2>/dev/null && cat {shlex.quote(STAMP_REL)} 2>/dev/null || true",
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise Failed(f"cannot reach remote {spec.name} ({spec.ssh}): {res.stderr.strip()}")
    stamp = res.stdout.strip()
    return stamp or None


def _invalidate_remote_stamp(spec: RemoteSpec) -> None:
    """Remove remote `var/.built_stamp` under remote lock before mutating sources."""
    inner = (
        f'd={_remote_cd_expr(spec.repo)}; '
        f'[ ! -d "$d" ] || (cd "$d" && mkdir -p var && '
        f'flock -o {shlex.quote(REMOTE_LOCK_REL)} rm -f {shlex.quote(STAMP_REL)})'
    )
    run([*_ssh_base_args(spec), f"bash -eu -c {shlex.quote(inner)}"])


def _ensure_remote_map_data(spec: RemoteSpec, repo: pathlib.Path = ROOT) -> None:
    """One-time push of static 3.3.5a client data (`out/server/data/`) guarded by `.synced`."""
    local_data = repo / "out" / "server" / "data"
    if not local_data.is_dir():
        raise Failed(f"local map data missing ({local_data}); run tools/wlb setup data first")
    probe = (
        f"cd {_remote_cd_expr(spec.repo)} 2>/dev/null && "
        f"test -f {shlex.quote(MAP_DATA_SENTINEL_REL)}"
    )
    res = subprocess.run([*_ssh_base_args(spec), probe], capture_output=True, text=True)
    if res.returncode == 0:
        return
    say(f"remote {spec.name}: pushing static 3.3.5a map data (one-time) -> {spec.ssh}:{spec.repo}/out/server/data")
    run([*_ssh_base_args(spec), f"mkdir -p {_remote_cd_expr(spec.repo)}/out/server/data"])
    run([
        "rsync", "-a", "--info=progress2", "-e", _rsync_ssh_cmd(), "--",
        f"{local_data}/", f"{spec.ssh}:{spec.repo}/out/server/data/",
    ])
    run([
        *_ssh_base_args(spec),
        f"cd {_remote_cd_expr(spec.repo)} && touch {shlex.quote(MAP_DATA_SENTINEL_REL)}",
    ])


def _verify_remote_sync_target(spec: RemoteSpec) -> None:
    """Verify remote target before `rsync --delete`: refuse `$HOME` or ancestors, require sentinel."""
    _validate_remote_repo(spec.repo)
    guard = (
        f'd={_remote_cd_expr(spec.repo)}; '
        'if [ ! -d "$d" ]; then '
        '  echo "remote sync target does not exist: $d (create it and touch .wlb-remote-repo first)" >&2; '
        '  exit 2; '
        'fi; '
        'TARGET_ABS=$(cd "$d" && pwd -P); '
        'HOME_ABS=$(cd "$HOME" && pwd -P); '
        'if [ "$TARGET_ABS" = "/" ] || [ "$TARGET_ABS" = "$HOME_ABS" ]; then '
        '  echo "refusing rsync --delete into $TARGET_ABS (equals / or \\$HOME)" >&2; '
        '  exit 2; '
        'fi; '
        'case "$HOME_ABS" in '
        '  "$TARGET_ABS"/*) '
        '    echo "refusing rsync --delete into $TARGET_ABS (ancestor of \\$HOME)" >&2; '
        '    exit 2 ;; '
        'esac; '
        f'if [ ! -f "$TARGET_ABS/tools/wlb" ] && [ ! -f "$TARGET_ABS/{REPO_SENTINEL_REL}" ]; then '
        f'  echo "refusing rsync --delete into $TARGET_ABS: missing tools/wlb or {REPO_SENTINEL_REL} sentinel" >&2; '
        '  exit 2; '
        'fi'
    )
    run([*_ssh_base_args(spec), f"bash -eu -c {shlex.quote(guard)}"])


def _rsync_to_remote(spec: RemoteSpec, repo: pathlib.Path = ROOT) -> None:
    _verify_remote_sync_target(spec)
    inner_rsync = (
        f'cd {_remote_cd_expr(spec.repo)} && mkdir -p var && '
        f'flock -o {shlex.quote(REMOTE_LOCK_REL)} rsync "$@"'
    )
    rsync_path = f"bash -c {shlex.quote(inner_rsync)} _"
    cmd = ["rsync", "-a", "--delete", f"--rsync-path={rsync_path}", "-e", _rsync_ssh_cmd()]
    for pat in RSYNC_EXCLUDES:
        cmd.append(f"--exclude={pat}")
    cmd += ["--", f"{repo}/", f"{spec.ssh}:{spec.repo}/"]
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

    _invalidate_remote_stamp(spec)
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

    inner_script = (
        f"cd {_remote_cd_expr(spec.repo)} && "
        f"mkdir -p var && flock -o {shlex.quote(REMOTE_LOCK_REL)} bash -euo pipefail -c "
        + shlex.quote(" && ".join(build_steps))
    )
    say(f"remote {spec.name}: building ({'common + server-test' if server_build else 'common'})...")
    run([*_ssh_base_args(spec), f"bash -euo pipefail -c {shlex.quote(inner_script)}"])
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
    probe_inner = (
        f'REPO_ABS=$(cd {_remote_cd_expr(spec.repo)} 2>/dev/null && pwd -P) || exit 0; '
        'found=0; '
        'for p in /proc/[0-9]*; do '
        '  pid=${p#/proc/}; [ "$pid" = "$$" ] || [ "$pid" = "$PPID" ] && continue; '
        '  args=$(tr "\\0" " " < "$p/cmdline" 2>/dev/null || true); '
        '  case "$args" in *worldserver*|*mysqld*|*harness*|*bench_bosses*) ;; *) continue ;; esac; '
        '  cwd=$(readlink -f "$p/cwd" 2>/dev/null || true); '
        '  exe=$(readlink -f "$p/exe" 2>/dev/null || true); '
        '  match=0; '
        '  case "$cwd" in "$REPO_ABS"|"$REPO_ABS"/*) match=1 ;; esac; '
        '  case "$exe" in "$REPO_ABS"|"$REPO_ABS"/*) match=1 ;; esac; '
        '  case "$args" in *"$REPO_ABS/"*|*"$REPO_ABS "*) match=1 ;; esac; '
        '  [ "$match" = 1 ] || continue; '
        '  printf "%s %s\\n" "$pid" "$args"; found=1; '
        'done; '
        '[ "$found" = 1 ] || echo "(no repo server/harness processes running)"'
    )
    res = subprocess.run(
        [*_ssh_base_args(spec), f"bash -c {shlex.quote(probe_inner)}"],
        capture_output=True, text=True,
    )
    if res.stdout.strip():
        say("  processes:")
        for line in res.stdout.strip().splitlines():
            say(f"    {line.strip()}")


def stop(name: str) -> None:
    spec = get_remote(name)
    with _remote_lock(spec.name):
        # Intentionally does NOT block on REMOTE_LOCK_REL so `stop` can always
        # halt a stuck test server/db even if another session holds the lock.
        inner = (
            f"cd {_remote_cd_expr(spec.repo)} && "
            "(tools/wlb server stop --test || true) && "
            "(tools/wlb db stop || true)"
        )
        run([*_ssh_base_args(spec), f"bash -c {shlex.quote(inner)}"])
        say(f"remote {spec.name}: stopped test server and db")


def _validate_remote_rel(remote_rel: str) -> None:
    p = pathlib.PurePosixPath(remote_rel)
    if not remote_rel or p.is_absolute() or ".." in p.parts:
        raise Failed(f"remote path must be relative inside repo: {remote_rel!r}")


def pull(name: str, remote_rel: str, local_path: str | pathlib.Path) -> None:
    _validate_remote_rel(remote_rel)
    spec = get_remote(name)
    dest = pathlib.Path(local_path)
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Omit -s (--protect-args) so leading ~/ in spec.repo expands on the remote
    # shell; shell-quote remote_rel so spaces/special chars stay safe.
    run([
        "rsync", "-a", "-e", _rsync_ssh_cmd(), "--",
        f"{spec.ssh}:{spec.repo}/{shlex.quote(remote_rel)}", str(dest),
    ])
    say(f"remote {spec.name}: pulled {remote_rel} -> {dest}")


def _parse_pull_spec(spec_str: str) -> tuple[str, pathlib.Path]:
    if ":" not in spec_str:
        raise Failed(f"--pull must be REMOTE_REL:LOCAL_PATH, got {spec_str!r}")
    remote_rel, local_str = spec_str.split(":", 1)
    if not remote_rel or not local_str:
        raise Failed(f"--pull must be REMOTE_REL:LOCAL_PATH, got {spec_str!r}")
    _validate_remote_rel(remote_rel)
    return remote_rel, pathlib.Path(local_str)


def _kill_remote_pgid(
    spec: RemoteSpec,
    *,
    pgid_rel: str = ACTIVE_PGID_REL,
    waiter_rel: str = "",
    only_if_active: bool = False,
) -> None:
    """Terminate a remote process group (and any pre-lock flock waiter) cleanly.

    Uses per-invocation `waiter_rel` / `pgid_rel` files so a queued client
    interrupted while waiting on `REMOTE_LOCK_REL` kills its own waiting `flock`
    without ever touching another client's active job or test server.
    """
    stop_test = "(tools/wlb server stop --test || true)"
    waiter_cleanup = ""
    if waiter_rel:
        waiter_cleanup = (
            f"if [ -f {shlex.quote(waiter_rel)} ]; then "
            f"  WPID=$(cat {shlex.quote(waiter_rel)} 2>/dev/null || true); "
            '  case "$WPID" in ""|*[!0-9]*) WPID="" ;; *) [ "$WPID" -gt 1 ] || WPID="" ;; esac; '
            '  if [ -n "$WPID" ]; then '
            '    kill -TERM -"$WPID" "$WPID" 2>/dev/null || true; '
            '    for _ in 1 2 3; do kill -0 -"$WPID" 2>/dev/null || break; sleep 1; done; '
            '    kill -KILL -"$WPID" "$WPID" 2>/dev/null || true; '
            "  fi; "
            f"  rm -f {shlex.quote(waiter_rel)}; "
            "fi; "
        )
    if only_if_active:
        cleanup_inner = (
            f"cd {_remote_cd_expr(spec.repo)} 2>/dev/null || exit 0; "
            f"{waiter_cleanup}"
            f"if [ -f {shlex.quote(pgid_rel)} ]; then "
            f"  PGID=$(cat {shlex.quote(pgid_rel)} 2>/dev/null || true); "
            '  case "$PGID" in ""|*[!0-9]*) PGID="" ;; *) [ "$PGID" -gt 1 ] || PGID="" ;; esac; '
            '  if [ -n "$PGID" ]; then '
            '    kill -TERM -"$PGID" 2>/dev/null || true; '
            '    for _ in 1 2 3 4 5; do kill -0 -"$PGID" 2>/dev/null || break; sleep 1; done; '
            '    kill -KILL -"$PGID" 2>/dev/null || true; '
            "  fi; "
            f"  rm -f {shlex.quote(pgid_rel)}; "
            f"  {stop_test}; "
            "fi"
        )
    else:
        cleanup_inner = (
            f"cd {_remote_cd_expr(spec.repo)} 2>/dev/null || exit 0; "
            f"{waiter_cleanup}"
            f"if [ -f {shlex.quote(pgid_rel)} ]; then "
            f"  PGID=$(cat {shlex.quote(pgid_rel)} 2>/dev/null || true); "
            '  case "$PGID" in ""|*[!0-9]*) PGID="" ;; *) [ "$PGID" -gt 1 ] || PGID="" ;; esac; '
            '  if [ -n "$PGID" ]; then '
            '    kill -TERM -"$PGID" 2>/dev/null || true; '
            '    for _ in 1 2 3 4 5; do kill -0 -"$PGID" 2>/dev/null || break; sleep 1; done; '
            '    kill -KILL -"$PGID" 2>/dev/null || true; '
            "  fi; "
            f"  rm -f {shlex.quote(pgid_rel)}; "
            "fi; "
            f"{stop_test}"
        )
    subprocess.run([*_ssh_base_args(spec), f"bash -c {shlex.quote(cleanup_inner)}"], check=False)


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

        # Run inside a single `setsid --wait` session with remote `flock -o`.
        # Pass the outer session PGID (`WLB_PGID=$$`) into `locked_body` and
        # write `pgid_rel` BEFORE removing `waiter_rel` so there is zero gap
        # window and `kill -TERM -"$PGID"` always signals the true session PGID.
        token = secrets.token_hex(6)
        waiter_rel = f"var/.waiter_remote_pgid.{token}"
        pgid_rel = f"{ACTIVE_PGID_REL}.{token}"
        locked_body = (
            f'echo "$WLB_PGID" > {shlex.quote(pgid_rel)} && '
            f"rm -f {shlex.quote(waiter_rel)} && "
            f"trap 'rm -f {shlex.quote(pgid_rel)}' EXIT && "
            f"{shlex.join(cmd)}"
        )
        inner = (
            f"cd {_remote_cd_expr(spec.repo)} && "
            f"mkdir -p var && WLB_PGID=$$ && export WLB_PGID && "
            f'echo "$WLB_PGID" > {shlex.quote(waiter_rel)} && '
            f"trap 'rm -f {shlex.quote(waiter_rel)}' EXIT && "
            f"flock -o {shlex.quote(REMOTE_LOCK_REL)} "
            f"bash -c {shlex.quote(locked_body)}"
        )
        remote_cmd = f"setsid --wait bash -c {shlex.quote(inner)}"
        ssh_cmd = [*_ssh_base_args(spec, tunnel_port=tunnel_port), remote_cmd]
        try:
            proc = subprocess.run(ssh_cmd)
        except KeyboardInterrupt:
            say(f"\nremote {spec.name}: interrupted; killing remote process group if active...")
            _kill_remote_pgid(spec, pgid_rel=pgid_rel, waiter_rel=waiter_rel, only_if_active=True)
            raise Failed("interrupted")

        if proc.returncode == 255:
            say(f"remote {spec.name}: exit 255 (checking for orphaned remote process group)...")
            _kill_remote_pgid(spec, pgid_rel=pgid_rel, waiter_rel=waiter_rel, only_if_active=True)

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
    """Install an untracked git `post-commit` hook (worktree-safe) that pre-warms `name`."""
    spec = get_remote(name)
    hook_rel = output(["git", "-C", str(repo), "rev-parse", "--git-path", "hooks/post-commit"]).strip()
    hook_path = pathlib.Path(hook_rel)
    if not hook_path.is_absolute():
        hook_path = (repo / hook_path).resolve()
    if hook_path.exists():
        existing = hook_path.read_text()
        if HOOK_MARKER not in existing:
            raise Failed(
                f"{hook_path} already exists and was not created by wlb remote; edit it manually"
            )
    log_path = repo / "var" / "remote" / f"{spec.name}-sync.log"
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


def parse_remote_argv(argv: list[str]) -> dict:
    """Strict CLI parser for `tools/wlb remote ...` so typos never get silently discarded."""
    if not argv or argv[0] in ("-h", "--help"):
        return {"help": True}
    target = argv[0]
    rest = argv[1:]
    if target == "list":
        if rest:
            raise Failed(f"unexpected arguments to `tools/wlb remote list`: {rest}")
        return {"help": False, "target": "list", "action": None}
    if not rest:
        raise Failed(
            f"tools/wlb remote {target} requires an action: {'|'.join(_VALID_ACTIONS)}"
        )
    action = rest[0]
    if action not in _VALID_ACTIONS:
        raise Failed(f"unknown remote action {action!r} (expected one of: {', '.join(_VALID_ACTIONS)})")
    tokens = rest[1:]

    opts: dict = {
        "help": False,
        "target": target,
        "action": action,
        "force": False,
        "no_sync": False,
        "no_tunnel": False,
        "no_server_build": False,
        "pull": [],
        "cmd": [],
    }
    if action in ("status", "stop", "install-hook"):
        if tokens:
            raise Failed(f"unexpected arguments to `tools/wlb remote {target} {action}`: {tokens}")
        return opts

    if action == "sync":
        for tok in tokens:
            if tok == "--force":
                opts["force"] = True
            elif tok == "--no-server-build":
                opts["no_server_build"] = True
            else:
                raise Failed(f"unknown option/argument for `remote sync`: {tok!r}")
        return opts

    if action == "pull":
        if len(tokens) != 2 or any(t.startswith("-") for t in tokens):
            raise Failed("usage: tools/wlb remote <name> pull REMOTE_REL LOCAL_PATH")
        opts["cmd"] = tokens
        return opts

    # action == "run"
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok == "--":
            i += 1
            break
        if not tok.startswith("-"):
            break
        if tok == "--no-sync":
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
            raise Failed(f"unknown option for `remote run`: {tok!r} (put `--` before remote command flags)")
    opts["cmd"] = tokens[i:]
    return opts


def dispatch_cli(argv: list[str]) -> None:
    parsed = parse_remote_argv(argv)
    if parsed["help"]:
        say(
            "usage:\n"
            "  tools/wlb remote list\n"
            "  tools/wlb remote <name> sync [--force] [--no-server-build]\n"
            "  tools/wlb remote <name> status | stop | install-hook\n"
            "  tools/wlb remote <name> pull REMOTE_REL LOCAL_PATH\n"
            "  tools/wlb remote <name> run [--no-sync] [--no-tunnel] [--no-server-build]\n"
            "                              [--pull REMOTE_REL:LOCAL_PATH ...] [--] <cmd...>"
        )
        return
    if parsed["target"] == "list":
        list_remotes()
        return
    target, action = parsed["target"], parsed["action"]
    if action == "sync":
        sync(target, force=parsed["force"], server_build=not parsed["no_server_build"])
    elif action == "status":
        status(target)
    elif action == "stop":
        stop(target)
    elif action == "install-hook":
        install_hook(target)
    elif action == "pull":
        pull(target, parsed["cmd"][0], parsed["cmd"][1])
    else:
        run_remote(
            target,
            parsed["cmd"],
            no_sync=parsed["no_sync"],
            no_tunnel=parsed["no_tunnel"],
            server_build=not parsed["no_server_build"],
            pulls=parsed["pull"],
        )
