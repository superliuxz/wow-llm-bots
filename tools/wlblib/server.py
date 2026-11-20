"""`wlb db`, `wlb config`, `wlb server`, `wlb console`: the local MySQL, the rendered config, the
game server (authserver + worldserver) and its console. Everything acts on this checkout's
processes only, found by their pid files, never by name across the machine.

Two servers (common.Instance): the play server, and the isolated test server e2e and perf run on
while you play (worldserver only, its own port, run directory, logs and databases, reset for
every run from a golden copy: docs/plans/phase2.md, E). Functions take `inst`, the play server
by default."""

import datetime
import errno
import os
import pathlib
import re
import signal
import subprocess
import time

from wlblib.common import (ETC, LOGS, MYSQL_CNF, PLAY, ROOT, TEST, VAR, Failed, alive,
                           output, pid_file, run, running_pid, say)

SECRETS = VAR / "secrets"
DB_PASSWORD = SECRETS / "db-password"
HUMAN_ACCOUNT_FILE = ROOT / "private" / "human-account"
BACKUPS = VAR / "backups"
# The databases that hold state; acore_world is rebuilt from AzerothCore's SQL by dbimport.
STATE_DATABASES = ("acore_auth", "acore_characters", "acore_playerbots")
# The same, by name without the prefix; and every database a server uses.
STATE_NAMES = ("auth", "characters", "playerbots")
ALL_NAMES = (*STATE_NAMES, "world")
# The test server's state databases as they were right after it first set them up: every test
# run starts from a copy of these (the world database isn't written by tests: it stays).
GOLDEN_PREFIX = "acore_golden_"
GOLDEN_KEY = TEST.root / "golden-key"  # What the golden copy was made with (submodule commits).


def console_pipe(inst=PLAY):
    """The pipe worldserver reads its console from."""
    return inst.run / "worldserver.stdin"


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


def _tokens(inst, password):
    """What the @...@ placeholders of config/*.overrides stand for, for `inst`."""
    return {"@DB_PASSWORD@": password, "@ROOT@": str(ROOT), "@DB@": inst.db_prefix,
            "@LOGS@": str(inst.logs), "@RUN@": str(inst.run),
            "@WORLD_PORT@": str(inst.world_port)}


def _overrides(path, tokens):
    """`key = value` pairs of an .overrides file, with the placeholders filled in."""
    pairs = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            say(f"ignored (no '='): {path.name}: {line}")
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        for token, text in tokens.items():
            value = value.replace(token, text)
        pairs.append((key, value))
    return pairs


def _apply(dist, out, override_files, tokens):
    """out = dist with every overridden key set (replaced where it is, else appended)."""
    text = dist.read_text()
    for path in override_files:
        for key, value in _overrides(path, tokens):
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


def config(test=None, inst=PLAY):
    """Renders <inst>/etc/*.conf from AzerothCore's .dist templates plus config/*.overrides
    (and config/tests/<test>.*.overrides for a test)."""
    tokens = _tokens(inst, _db_password())
    configs = ROOT / "config"
    if test and not list((configs / "tests").glob(f"{test}.*.overrides")):
        raise Failed(f"no test config {test} (config/tests/{test}.*.overrides)")
    for template, names, test_name in CONFIGS:
        dist = inst.etc / template
        if not dist.exists():
            if "mod-llm-brain" in template:
                continue  # A server built without our module.
            raise Failed(f"no {template} under {inst.etc.relative_to(ROOT)} (tools/wlb build "
                         f"server{' --test' if inst is TEST else ''})")
        files = [configs / f"{name}.overrides" for name in names]
        if test and test_name:
            extra = configs / "tests" / f"{test}.{test_name}.overrides"
            if extra.exists():
                files.append(extra)
        _apply(dist, dist.with_suffix(""), files, tokens)
    say(f"{inst.name} config applied{f' (test: {test})' if test else ''}")


# ---- Server -------------------------------------------------------------------------------------

def online_characters(inst=PLAY):
    """The names of the characters online now (from the characters database)."""
    rows = output(["mysql", f"--defaults-file={MYSQL_CNF}", "-uroot", "-N", "-e",
                   f"SELECT name FROM {inst.db('characters')}.characters WHERE online=1"])
    return set(rows.split())


def wait_online_count(count, timeout_s=300, inst=PLAY):
    """Waits until at least `count` characters are online, or fails after `timeout_s`."""
    deadline = time.time() + timeout_s
    seen = 0
    while time.time() < deadline:
        try:
            seen = len(online_characters(inst))
            if seen >= count:
                return
        except Failed:
            pass  # MySQL still starting.
        time.sleep(3)
    raise Failed(f"only {seen} of {count} characters came online in {timeout_s} s")


def wait_online(name, timeout_s=180, inst=PLAY):
    """Waits until the character `name` is online, or fails after `timeout_s`."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not running_pid("worldserver", inst):
            raise Failed(f"the {inst.name} worldserver exited (log: "
                         f"{(inst.logs / 'worldserver.out').relative_to(ROOT)})")
        try:
            if name in online_characters(inst):
                return
        except Failed:
            pass  # MySQL still starting.
        time.sleep(2)
    raise Failed(f"{name} didn't come online in {timeout_s} s (log: "
                 f"{(inst.logs / 'worldserver.out').relative_to(ROOT)})")


def _start_detached(name, cmd, cwd, log, stdin=subprocess.DEVNULL, inst=PLAY):
    with open(log, "ab") as out:
        process = subprocess.Popen(cmd, cwd=cwd, stdin=stdin, stdout=out,
                                   stderr=subprocess.STDOUT, start_new_session=True)
    pid_file(name, inst).write_text(f"{process.pid}\n")
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


def server_start(inst=PLAY):
    """Starts MySQL, authserver and worldserver, all in the background. worldserver reads its
    console from a pipe (`wlb console ...`). The test server is a worldserver only (its bots are
    server-side: nobody logs in), on the client data the play server's build extracted."""
    play = inst is PLAY
    if not play and not (ROOT / "out" / "server" / "data").is_dir():
        raise Failed("no client data in out/server/data (tools/wlb setup data): the test "
                     "server shares the play server's")
    inst.run.mkdir(parents=True, exist_ok=True)
    # MySQL's socket (root, no password) and the brain and console pipes: this user only.
    inst.run.chmod(0o700)
    inst.logs.mkdir(parents=True, exist_ok=True)
    db_start()
    if play and not running_pid("authserver"):
        _start_detached("authserver", ["./authserver", "-c", "../etc/authserver.conf"],
                        inst.bin, LOGS / "authserver.out")
        say("authserver started")
    if not running_pid("worldserver", inst):
        pipe = console_pipe(inst)
        pipe.unlink(missing_ok=True)
        os.mkfifo(pipe, 0o600)
        # Hold the pipe open for writing, so worldserver never sees EOF on its console.
        holder = _hold_console_open(pipe)
        pid_file("worldserver-pipe", inst).write_text(f"{holder}\n")
        with open(pipe, "rb", buffering=0) as console:
            _start_detached("worldserver", ["./worldserver", "-c", "../etc/worldserver.conf"],
                            inst.bin, inst.logs / "worldserver.out", stdin=console, inst=inst)
        say(f"{inst.name} worldserver started (log: "
            f"{(inst.logs / 'worldserver.out').relative_to(ROOT)})")
    if not play:
        return
    account = _human_account()
    if account and not _sampler_running():
        (LOGS / "human").mkdir(parents=True, exist_ok=True)
        log = LOGS / "human" / f"state-{datetime.date.today():%Y%m%d}.jsonl"
        _start_detached("human-sampler",
                        [ROOT / "tools" / "wlb", "server", "sample", log, "60", account],
                        ROOT, os.devnull)
        say("human-play sampler started")


def _hold_console_open(pipe):
    """A `sleep infinity` holding the console pipe open for writing; returns its pid. Opening a
    pipe for writing blocks until a reader opens it, so it is opened non-blocking (O_RDWR on a
    FIFO never blocks on Linux) and handed to the child."""
    fd = os.open(pipe, os.O_RDWR | os.O_NONBLOCK)
    try:
        # Nothing of the caller's: an inherited stdin/stderr pipe would keep whoever reads
        # `wlb server start` output (a `| tail`) waiting for as long as the server runs.
        holder = subprocess.Popen(["sleep", "infinity"], stdin=subprocess.DEVNULL, stdout=fd,
                                  stderr=subprocess.DEVNULL, start_new_session=True)
    finally:
        os.close(fd)
    return holder.pid


def _sampler_running():
    try:
        pid = int(pid_file("human-sampler").read_text())
    except (OSError, ValueError):
        return False
    return alive(pid)


def console(command, wait_s=1.0, inst=PLAY):
    """Sends one command to the running worldserver's console and prints the log lines it
    produced. Never blocks: the pipe is written non-blocking, only while this checkout's
    worldserver is alive."""
    pipe = console_pipe(inst)
    if not running_pid("worldserver", inst) or not pipe.exists():
        raise Failed(f"the {inst.name} worldserver isn't running")
    log = inst.logs / "worldserver.out"
    start = log.stat().st_size
    try:
        fd = os.open(pipe, os.O_WRONLY | os.O_NONBLOCK)
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
        text = f.read().decode(errors="replace")
    print(text, end="")
    return text


def _stop_pid(name, timeout_s, inst=PLAY):
    """SIGTERM this checkout's `name`, wait up to `timeout_s`, then SIGKILL."""
    pid = running_pid(name, inst)
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
    pid_file(name, inst).unlink(missing_ok=True)


def _kill_pid_file(name, *cmdline_words, inst=PLAY):
    """SIGKILLs the process in `name`'s pid file, only if its command line still has all of
    `cmdline_words`: after a reboot or a long time the pid may belong to something else."""
    try:
        pid = int(pid_file(name, inst).read_text())
        args = pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    except (OSError, ValueError):
        pid, args = None, []
    words = [a.decode(errors="replace") for a in args]
    if pid and all(any(w in a for a in words) for w in cmdline_words):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    pid_file(name, inst).unlink(missing_ok=True)


def worldserver_stop(inst=PLAY):
    """Stops `inst`'s worldserver gracefully (it saves characters), SIGKILL after 90 s."""
    pid = running_pid("worldserver", inst)
    pipe = console_pipe(inst)
    if pid:
        try:
            fd = os.open(pipe, os.O_WRONLY | os.O_NONBLOCK)
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
            say(f"{inst.name} worldserver force-killed")
        else:
            say(f"{inst.name} worldserver stopped")
    pid_file("worldserver", inst).unlink(missing_ok=True)
    _kill_pid_file("worldserver-pipe", "sleep", "infinity", inst=inst)
    pipe.unlink(missing_ok=True)


def server_stop(backup=True, inst=PLAY):
    """Stops worldserver (gracefully: it saves characters), authserver, backs the databases up
    (unless `backup` is False), then stops MySQL. The test server is only its worldserver:
    MySQL stays up for the play server."""
    if inst is not PLAY:
        worldserver_stop(inst)
        return
    _kill_pid_file("human-sampler", "server", "sample")
    worldserver_stop(PLAY)
    _stop_pid("authserver", 10)
    if _mysql_up():
        try:
            if backup:
                db_backup()  # The databases are quiet now.
        except Failed as e:
            say(f"backup FAILED (the server is stopping anyway): {e}")
        if running_pid("worldserver", TEST):
            say("the test server is running: MySQL stays up for it")
        else:
            db_stop()


# ---- The test server's databases ----------------------------------------------------------------

def _sql(statement):
    return output(["mysql", f"--defaults-file={MYSQL_CNF}", "-uroot", "-N", "-B", "-e",
                   statement])


def _databases():
    return set(_sql("SHOW DATABASES").split())


def _copy_database(source, target):
    """Replaces database `target` with a copy of `source` (tables and rows), inside MySQL."""
    tables = _sql(f"SELECT table_name FROM information_schema.tables WHERE "
                  f"table_schema='{source}' AND table_type='BASE TABLE'").split()
    statements = [f"DROP DATABASE IF EXISTS `{target}`", f"CREATE DATABASE `{target}`",
                  "SET SESSION foreign_key_checks=0"]
    for table in tables:
        statements += [f"CREATE TABLE `{target}`.`{table}` LIKE `{source}`.`{table}`",
                       f"INSERT INTO `{target}`.`{table}` SELECT * FROM `{source}`.`{table}`"]
    _sql(";\n".join(statements))


def _golden_key():
    """What the golden copy depends on: the commits of AzerothCore and playerbots, whose SQL
    sets the databases up."""
    third = ROOT / "third_party"
    return " ".join(output(["git", "-C", third / name, "rev-parse", "HEAD"]).strip()
                    for name in ("azerothcore-wotlk", "mod-playerbots"))


def _grant(name):
    for host in ("127.0.0.1", "localhost"):
        _sql(f"GRANT ALL PRIVILEGES ON `{name}`.* TO 'acore'@'{host}'")


def test_databases_reset():
    """Puts the test server's databases in their golden state: auth, characters and playerbots
    copied from the golden copy; world as the test server set it up. The first time (and when
    AzerothCore or playerbots moved on), the test server is started once on its databases to set
    them up (a few minutes for an empty world database), and the golden copy is made from the
    result. The test server must be stopped."""
    db_start()
    have = _databases()
    golden = all(GOLDEN_PREFIX + n in have for n in STATE_NAMES)
    key = _golden_key()
    try:
        current = GOLDEN_KEY.read_text().strip()
    except OSError:
        current = None
    if not golden or current != key:
        say("making the test server's golden databases (first run, or AzerothCore or "
            "playerbots moved on)")
        for name in ALL_NAMES:
            db = TEST.db(name)
            if golden:
                if name in STATE_NAMES:  # Start from the old golden: only updates to apply.
                    _copy_database(GOLDEN_PREFIX + name, db)
            elif db not in have:
                _sql(f"CREATE DATABASE `{db}`")
            _grant(db)
        GOLDEN_KEY.unlink(missing_ok=True)
        _set_up_once()
        for name in STATE_NAMES:
            _copy_database(TEST.db(name), GOLDEN_PREFIX + name)
        GOLDEN_KEY.write_text(key + "\n")
        say("golden databases made")
    for name in STATE_NAMES:
        _copy_database(GOLDEN_PREFIX + name, TEST.db(name))
    say("test databases reset")


def _set_up_once(timeout_s=1800):
    """Starts the test server on the base config (no test characters) until it is ready, so it
    sets its databases up (AzerothCore's and playerbots' updaters), then stops it."""
    config(inst=TEST)
    log = TEST.logs / "worldserver.out"
    TEST.logs.mkdir(parents=True, exist_ok=True)
    start = log.stat().st_size if log.exists() else 0
    server_start(TEST)
    deadline = time.time() + timeout_s
    try:
        while True:
            with open(log, "rb") as f:
                f.seek(start)
                if b") ready..." in f.read():
                    break
            if not running_pid("worldserver", TEST):
                raise Failed(f"the test worldserver exited while setting up its databases "
                             f"(log: {log.relative_to(ROOT)})")
            if time.time() > deadline:
                raise Failed(f"the test worldserver wasn't ready in {timeout_s} s "
                             f"(log: {log.relative_to(ROOT)})")
            time.sleep(5)
    finally:
        worldserver_stop(TEST)


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
