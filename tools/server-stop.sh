#!/usr/bin/env bash
# Stop worldserver (graceful), authserver, then MySQL.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUN=$ROOT/out/server/run
stop() {  # stop <name> <timeout_s>
  local f=$RUN/$1.pid; [ -f "$f" ] || return 0
  local pid; pid=$(cat "$f")
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid"
    for _ in $(seq 1 "$2"); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid"
    echo "$1 stopped"
  fi
  rm -f "$f"
}
stop human-sampler 1

# Graceful world shutdown via console (saves characters), then wait for the process to exit.
if pgrep -u "$(id -u)" -x worldserver >/dev/null && [ -p "$RUN/worldserver.stdin" ]; then
  echo "server shutdown 1" > "$RUN/worldserver.stdin"
fi
for _ in $(seq 1 90); do pgrep -u "$(id -u)" -x worldserver >/dev/null || break; sleep 1; done
pkill -KILL -u "$(id -u)" -x worldserver 2>/dev/null && echo "worldserver force-killed" || echo "worldserver stopped"
rm -f "$RUN/worldserver.pid"
stop worldserver-pipe 1
rm -f "$RUN/worldserver.stdin"
pkill -TERM -u "$(id -u)" -x authserver && echo "authserver stopped"; rm -f "$RUN/authserver.pid"
for _ in $(seq 1 10); do pgrep -u "$(id -u)" -x authserver >/dev/null || break; sleep 1; done
# The databases are quiet now: back them up before MySQL stops (tools/db-backup.sh).
"$ROOT/tools/db-backup.sh" || echo "backup FAILED (the server is stopping anyway)"
"$ROOT/tools/db-stop.sh"
