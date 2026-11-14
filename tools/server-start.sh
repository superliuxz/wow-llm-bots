#!/usr/bin/env bash
# Start MySQL, authserver and worldserver (all in the background).
# worldserver's console reads from a named pipe: send commands with tools/wsc.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
S=$ROOT/server; RUN=$S/run; LOG=$S/logs
mkdir -p "$RUN" "$LOG"
"$ROOT/tools/db-start.sh"
running() { [ -f "$RUN/$1.pid" ] && kill -0 "$(cat "$RUN/$1.pid")" 2>/dev/null; }

if ! running authserver; then
  cd "$S/bin" && nohup ./authserver -c ../etc/authserver.conf > "$LOG/authserver.out" 2>&1 &
  echo $! > "$RUN/authserver.pid"; echo "authserver started"
fi

if ! running worldserver; then
  rm -f "$RUN/worldserver.stdin"; mkfifo "$RUN/worldserver.stdin"
  # Hold the pipe open so worldserver never sees EOF on its console.
  nohup sleep infinity > "$RUN/worldserver.stdin" 2>/dev/null &
  echo $! > "$RUN/worldserver-pipe.pid"
  cd "$S/bin" && nohup ./worldserver -c ../etc/worldserver.conf < "$RUN/worldserver.stdin" > "$LOG/worldserver.out" 2>&1 &
  echo $! > "$RUN/worldserver.pid"; echo "worldserver started (log: $LOG/worldserver.out)"
fi
