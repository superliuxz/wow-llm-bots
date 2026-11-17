#!/usr/bin/env bash
# Start MySQL, authserver and worldserver (all in the background).
# worldserver's console reads from a named pipe: send commands with tools/wsc.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
S=$ROOT/out/server; RUN=$S/run; LOG=$ROOT/var/logs
mkdir -p "$RUN" "$LOG"
# The run directory holds MySQL's socket (root, no password) and the brain and console pipes:
# this user only.
chmod 700 "$RUN"
"$ROOT/tools/db-start.sh"
# This checkout's server only: its pid file, and that pid really is the program (a stale file
# whose pid was reused by something else doesn't count).
running() {
  local pid; pid=$(cat "$RUN/$1.pid" 2>/dev/null) || return 1
  [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$1" ]
}

if ! running authserver; then
  (cd "$S/bin" && exec ./authserver -c ../etc/authserver.conf) < /dev/null > "$LOG/authserver.out" 2>&1 &
  echo $! > "$RUN/authserver.pid"; echo "authserver started"
fi

if ! running worldserver; then
  rm -f "$RUN/worldserver.stdin"; mkfifo "$RUN/worldserver.stdin"
  # Hold the pipe open so worldserver never sees EOF on its console.
  (exec sleep infinity) > "$RUN/worldserver.stdin" 2>/dev/null &
  echo $! > "$RUN/worldserver-pipe.pid"
  (cd "$S/bin" && exec ./worldserver -c ../etc/worldserver.conf) < "$RUN/worldserver.stdin" > "$LOG/worldserver.out" 2>&1 &
  echo $! > "$RUN/worldserver.pid"; echo "worldserver started (log: $LOG/worldserver.out)"
fi

# Human baseline for evals: sample the human player's character state while online (untracked,
# var/logs/human/). Their account name is personal: it lives in private/human-account (one line),
# never in this repo; without that file nothing is sampled.
HUMAN_ACCOUNT_FILE=$ROOT/private/human-account
# An account name is letters and digits only (it goes into the sampler's SQL LIKE).
HUMAN_ACCOUNT=$(head -n1 "$HUMAN_ACCOUNT_FILE" 2>/dev/null | tr -d '[:space:]' || true)
if [ -n "$HUMAN_ACCOUNT" ] && ! [[ "$HUMAN_ACCOUNT" =~ ^[A-Za-z0-9]+$ ]]; then
  echo "private/human-account: not an account name, sampler not started" >&2; HUMAN_ACCOUNT=
fi
if [ -n "$HUMAN_ACCOUNT" ] && ! { [ -f "$RUN/human-sampler.pid" ] && kill -0 "$(cat "$RUN/human-sampler.pid")" 2>/dev/null; }; then
  mkdir -p "$LOG/human"
  (exec "$ROOT/tools/bot-sampler.sh" "$LOG/human/state-$(date +%Y%m%d).jsonl" 60 "$HUMAN_ACCOUNT") > /dev/null 2>&1 &
  echo $! > "$RUN/human-sampler.pid"; echo "human-play sampler started"
fi
