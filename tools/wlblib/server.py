"""`wlb db`, `wlb config`, `wlb server`, `wlb console`: the local MySQL, the rendered config, the
game server (authserver + worldserver) and its console. Everything acts on this checkout's
processes only, found by their pid files, never by name across the machine."""

import datetime
import errno
import os
import re
import signal
import subprocess
import time

from wlblib.common import (ETC, LOGS, MYSQL_CNF, ROOT, RUN, SERVER, VAR, Failed, alive,
                           output, pid_file, run, running_pid, say)

CONSOLE = RUN / "worldserver.stdin"  # worldserver reads its console from this pipe.
SECRETS = VAR / "secrets"
DB_PASSWORD = SECRETS / "db-password"
HUMAN_ACCOUNT_FILE = ROOT / "private" / "human-account"
BACKUPS = VAR / "backups"
# The databases that hold state; acore_world is rebuilt from AzerothCore's SQL by dbimport.
STATE_DATABASES = ("acore_auth", "acore_characters", "acore_playerbots")


# ---- MySQL --------------------------------------------------------------------------------------

def _mysql_up():
    return subprocess.run(["mysqladmin", f"--defaults-file={MYSQL_CNF}", "-uroot", "ping"],
                          capture_output=True).returncode == 0


def db_start():
    """Starts the project-local MySQL (Percona) in the background, from config/mysql.cnf.in."""
    ETC.mkdir(parents=True, exist_ok=True)
    MYSQL_CNF.write_text((ROOT / "config" / "mysql.cnf.in").read_text()
                         .replace("@ROOT@", str(ROOT)))
    if _mysql_up():
        say("mysql already running")
        return
    run(["mysqld", f"--defaults-file={MYSQL_CNF}", "--daemonize"])
    for _ in range(30):
        if _mysql_up():
            say("mysql up")
            return
        time.sleep(1)
    raise Failed(f"mysql failed to start; see {LOGS / 'mysql.log'}")


def db_stop():
    run(["mysqladmin", f"--defaults-file={MYSQL_CNF}", "-uroot", "shutdown"])
    say("mysql stopped")


def db_backup(keep=14):
    """Backs up the databases that hold state (accounts, characters, playerbots' tables),
    compressed, into var/backups/db-<time>.sql.zst, keeping the newest `keep`.

    Restore (replaces those databases; stop the world server first):
      zstd -dc var/backups/db-<time>.sql.zst | mysql --defaults-file=out/server/etc/mysql.cnf -uroot
    """
    if not _mysql_up():
        raise Failed("mysql is not running (tools/wlb db start)")
    BACKUPS.mkdir(parents=True, exist_ok=True)
    out = BACKUPS / f"db-{datetime.datetime.now():%Y%m%d-%H%M%S}.sql.zst"
    tmp = out.with_name(out.name + ".tmp")
    try:
        dump = subprocess.Popen(
            ["mysqldump", f"--defaults-file={MYSQL_CNF}", "-uroot", "--single-transaction",
             "--routines", "--triggers", "--set-gtid-purged=OFF", "--databases",
             *STATE_DATABASES], stdout=subprocess.PIPE)
        zstd = subprocess.run(["zstd", "-q", "-T0", "-o", tmp], stdin=dump.stdout)
        dump.stdout.close()
        if dump.wait() != 0 or zstd.returncode != 0:
            raise Failed("backup failed (mysqldump or zstd)")
        tmp.rename(out)
    finally:
        tmp.unlink(missing_ok=True)
    for old in sorted(BACKUPS.glob("db-*.sql.zst"), reverse=True)[keep:]:
        old.unlink()
    say(f"backup: {out.relative_to(ROOT)} ({out.stat().st_size / 1e6:.0f} MB)")


# ---- Config -------------------------------------------------------------------------------------

def _db_password():
    if not DB_PASSWORD.exists():
        old = ETC / ".acore-db-password"  # Where it lived before var/secrets.
        if old.exists():
            SECRETS.mkdir(parents=True, exist_ok=True)
            SECRETS.chmod(0o700)
            old.rename(DB_PASSWORD)
            DB_PASSWORD.chmod(0o600)
    try:
        password = DB_PASSWORD.read_text().splitlines()[0].strip()
    except (OSError, IndexError):
        password = ""
    if not password:
        raise Failed(f"no DB password in {DB_PASSWORD.relative_to(ROOT)}: create it (one line, "
                     "mode 600) first")
    return password


def _overrides(path, password):
    """`key = value` pairs of an .overrides file; @DB_PASSWORD@ and @ROOT@ filled in."""
    pairs = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            say(f"ignored (no '='): {path.name}: {line}")
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        pairs.append((key, value.replace("@DB_PASSWORD@", password)
                      .replace("@ROOT@", str(ROOT))))
    return pairs


def _apply(dist, out, override_files, password):
    """out = dist with every overridden key set (replaced where it is, else appended)."""
    text = dist.read_text()
    for path in override_files:
        for key, value in _overrides(path, password):
            line = f"{key} = {value}"
            pattern = re.compile(rf"^{re.escape(key)}[ \t]*=.*$", re.M)
            if pattern.search(text):
                text = pattern.sub(lambda _: line, text, count=1)
            else:
                text += ("" if text.endswith("\n") else "\n") + line + "\n"
    out.write_text(text)
    out.chmod(0o600)


# The rendered config: template (under out/server/etc) -> the overrides applied to it, in order,
# and the name of its test overrides (config/tests/<test>.<name>.overrides), if any.
CONFIGS = (
    ("worldserver.conf.dist", ("common", "worldserver"), "worldserver"),
    ("authserver.conf.dist", ("common",), None),
    ("dbimport.conf.dist", ("common",), None),
    ("modules/playerbots.conf.dist", ("playerbots",), "playerbots"),
    ("modules/mod-llm-brain.conf.dist", ("llm-brain",), "llm-brain"),
)


def config(test=None):
    """Renders out/server/etc/*.conf from AzerothCore's .dist templates plus config/*.overrides
    (and config/tests/<test>.*.overrides for a test)."""
    password = _db_password()
    configs = ROOT / "config"
    if test and not list((configs / "tests").glob(f"{test}.*.overrides")):
        raise Failed(f"no test config {test} (config/tests/{test}.*.overrides)")
    for template, names, test_name in CONFIGS:
        dist = ETC / template
        if not dist.exists():
            if "mod-llm-brain" in template:
                continue  # A server built without our module.
            raise Failed(f"no {template} under {ETC.relative_to(ROOT)} (tools/wlb build server)")
        files = [configs / f"{name}.overrides" for name in names]
        if test and test_name:
            extra = configs / "tests" / f"{test}.{test_name}.overrides"
            if extra.exists():
                files.append(extra)
        _apply(dist, dist.with_suffix(""), files, password)
    say(f"config applied{f' (test: {test})' if test else ''}")


# ---- Server -------------------------------------------------------------------------------------

def _start_detached(name, cmd, cwd, log, stdin=subprocess.DEVNULL):
    with open(log, "ab") as out:
        process = subprocess.Popen(cmd, cwd=cwd, stdin=stdin, stdout=out,
                                   stderr=subprocess.STDOUT, start_new_session=True)
    pid_file(name).write_text(f"{process.pid}\n")
    return process.pid


def _human_account():
    """The human player's account name (personal: it lives in private/, never in this repo),
    or None. Letters and digits only: it goes into the sampler's SQL LIKE."""
    try:
        account = HUMAN_ACCOUNT_FILE.read_text().split()[0]
    except (OSError, IndexError):
        return None
    if not re.fullmatch(r"[A-Za-z0-9]+", account):
        say("private/human-account: not an account name, sampler not started")
        return None
    return account


def server_start():
    """Starts MySQL, authserver and worldserver, all in the background. worldserver reads its
    console from a pipe (`wlb console ...`)."""
    RUN.mkdir(parents=True, exist_ok=True)
    # MySQL's socket (root, no password) and the brain and console pipes: this user only.
    RUN.chmod(0o700)
    LOGS.mkdir(parents=True, exist_ok=True)
    db_start()
    bin_dir = SERVER / "bin"
    if not running_pid("authserver"):
        _start_detached("authserver", ["./authserver", "-c", "../etc/authserver.conf"],
                        bin_dir, LOGS / "authserver.out")
        say("authserver started")
    if not running_pid("worldserver"):
        CONSOLE.unlink(missing_ok=True)
        os.mkfifo(CONSOLE, 0o600)
        # Hold the pipe open for writing, so worldserver never sees EOF on its console.
        holder = _hold_console_open()
        pid_file("worldserver-pipe").write_text(f"{holder}\n")
        with open(CONSOLE, "rb", buffering=0) as console:
            _start_detached("worldserver", ["./worldserver", "-c", "../etc/worldserver.conf"],
                            bin_dir, LOGS / "worldserver.out", stdin=console)
        say(f"worldserver started (log: {(LOGS / 'worldserver.out').relative_to(ROOT)})")
    account = _human_account()
    if account and not _sampler_running():
        (LOGS / "human").mkdir(parents=True, exist_ok=True)
        log = LOGS / "human" / f"state-{datetime.date.today():%Y%m%d}.jsonl"
        _start_detached("human-sampler",
                        [ROOT / "tools" / "wlb", "server", "sample", log, "60", account],
                        ROOT, os.devnull)
        say("human-play sampler started")


def _hold_console_open():
    """A `sleep infinity` holding the console pipe open for writing; returns its pid. Opening a
    pipe for writing blocks until a reader opens it, so it is opened non-blocking (O_RDWR on a
    FIFO never blocks on Linux) and handed to the child."""
    fd = os.open(CONSOLE, os.O_RDWR | os.O_NONBLOCK)
    try:
        holder = subprocess.Popen(["sleep", "infinity"], stdout=fd, start_new_session=True)
    finally:
        os.close(fd)
    return holder.pid


def _sampler_running():
    try:
        pid = int(pid_file("human-sampler").read_text())
    except (OSError, ValueError):
        return False
    return alive(pid)


def console(command, wait_s=1.0):
    """Sends one command to the running worldserver's console and prints the log lines it
    produced. Never blocks: the pipe is written non-blocking, only while this checkout's
    worldserver is alive."""
    if not running_pid("worldserver") or not CONSOLE.exists():
        raise Failed("worldserver isn't running (tools/wlb server start)")
    log = LOGS / "worldserver.out"
    start = log.stat().st_size
    try:
        fd = os.open(CONSOLE, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as e:
        if e.errno == errno.ENXIO:  # No reader: worldserver isn't reading its console.
            raise Failed("worldserver isn't reading its console") from e
        raise
    try:
        os.write(fd, (command + "\n").encode())
    finally:
        os.close(fd)
    time.sleep(wait_s)
    with open(log, "rb") as f:
        f.seek(start)
        print(f.read().decode(errors="replace"), end="")


def _stop_pid(name, timeout_s):
    """SIGTERM this checkout's `name`, wait up to `timeout_s`, then SIGKILL."""
    pid = running_pid(name)
    if pid:
        os.kill(pid, signal.SIGTERM)
        for _ in range(timeout_s):
            if not alive(pid):
                break
            time.sleep(1)
        if alive(pid):
            os.kill(pid, signal.SIGKILL)
            say(f"{name} force-killed")
        else:
            say(f"{name} stopped")
    pid_file(name).unlink(missing_ok=True)


def _kill_pid_file(name):
    try:
        os.kill(int(pid_file(name).read_text()), signal.SIGKILL)
    except (OSError, ValueError):
        pass
    pid_file(name).unlink(missing_ok=True)


def server_stop():
    """Stops worldserver (gracefully: it saves characters), authserver, backs the databases up,
    then stops MySQL."""
    _kill_pid_file("human-sampler")
    pid = running_pid("worldserver")
    if pid:
        try:
            fd = os.open(CONSOLE, os.O_WRONLY | os.O_NONBLOCK)
            os.write(fd, b"server shutdown 1\n")
            os.close(fd)
        except OSError:
            pass  # Not reading its console: it gets SIGKILL below.
        for _ in range(90):
            if not alive(pid):
                break
            time.sleep(1)
        if alive(pid):
            os.kill(pid, signal.SIGKILL)
            say("worldserver force-killed")
        else:
            say("worldserver stopped")
    pid_file("worldserver").unlink(missing_ok=True)
    _kill_pid_file("worldserver-pipe")
    CONSOLE.unlink(missing_ok=True)
    _stop_pid("authserver", 10)
    if _mysql_up():
        try:
            db_backup()  # The databases are quiet now.
        except Failed as e:
            say(f"backup FAILED (the server is stopping anyway): {e}")
        db_stop()


def sample(out, interval_s, account_like):
    """Every `interval_s`, appends the state of the online characters of accounts matching
    `account_like` (SQL LIKE) to `out` as JSON lines. Runs until killed."""
    if not re.fullmatch(r"[A-Za-z0-9%_]+", account_like):
        raise Failed(f"not an account pattern: {account_like!r}")
    query = f"""SELECT JSON_OBJECT('t', UNIX_TIMESTAMP(), 'name', c.name, 'race', c.race,
      'class', c.class, 'level', c.level, 'xp', c.xp, 'money', c.money, 'map', c.map,
      'zone', c.zone, 'x', ROUND(c.position_x,1), 'y', ROUND(c.position_y,1),
      'z', ROUND(c.position_z,1), 'online', c.online, 'played', c.totaltime,
      'q_active', (SELECT COUNT(*) FROM acore_characters.character_queststatus q WHERE q.guid=c.guid),
      'q_rewarded', (SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r
                     WHERE r.guid=c.guid),
      'dead', (SELECT COUNT(*) FROM acore_characters.corpse k WHERE k.guid=c.guid))
    FROM acore_characters.characters c JOIN acore_auth.account a ON a.id=c.account
    WHERE a.username LIKE '{account_like}' AND c.online = 1"""
    out = os.path.abspath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    while True:
        try:
            rows = output(["mysql", f"--defaults-file={MYSQL_CNF}", "-uroot", "-N", "-B",
                           "-e", query])
            with open(out, "a") as f:
                f.write(rows)
        except Failed:
            pass  # MySQL down for a moment: try again next time.
        time.sleep(float(interval_s))
