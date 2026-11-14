#!/usr/bin/env bash
# Stop worldserver (graceful), authserver, then MySQL.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUN=$ROOT/server/run
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
[ -p "$RUN/worldserver.stdin" ] && kill -0 "$(cat "$RUN/worldserver.pid" 2>/dev/null)" 2>/dev/null \
  && echo "server shutdown 1" > "$RUN/worldserver.stdin"
stop worldserver 60
stop worldserver-pipe 1
rm -f "$RUN/worldserver.stdin"
stop authserver 10
"$ROOT/tools/db-stop.sh"
