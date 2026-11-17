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
# Only if this checkout's worldserver (its pid file) is alive: writing to the pipe blocks when
# nothing reads it, so the write also has a timeout.
WS_PID=$(cat "$RUN/worldserver.pid" 2>/dev/null || true)
if [ -n "$WS_PID" ] && kill -0 "$WS_PID" 2>/dev/null && [ -p "$RUN/worldserver.stdin" ]; then
  timeout 5 bash -c 'echo "server shutdown 1" > "$1"' _ "$RUN/worldserver.stdin" || true
fi
# This checkout's worldserver only (by its pid), never another checkout's.
if [ -n "$WS_PID" ]; then
  for _ in $(seq 1 90); do kill -0 "$WS_PID" 2>/dev/null || break; sleep 1; done
  kill -KILL "$WS_PID" 2>/dev/null && echo "worldserver force-killed" || echo "worldserver stopped"
fi
rm -f "$RUN/worldserver.pid"
stop worldserver-pipe 1
rm -f "$RUN/worldserver.stdin"
AS_PID=$(cat "$RUN/authserver.pid" 2>/dev/null || true)
if [ -n "$AS_PID" ] && kill -TERM "$AS_PID" 2>/dev/null; then
  for _ in $(seq 1 10); do kill -0 "$AS_PID" 2>/dev/null || break; sleep 1; done
  kill -KILL "$AS_PID" 2>/dev/null && echo "authserver force-killed" || echo "authserver stopped"
fi
rm -f "$RUN/authserver.pid"
# The databases are quiet now: back them up before MySQL stops (tools/db-backup.sh).
"$ROOT/tools/db-backup.sh" || echo "backup FAILED (the server is stopping anyway)"
"$ROOT/tools/db-stop.sh"
