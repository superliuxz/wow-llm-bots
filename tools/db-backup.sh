#!/usr/bin/env bash
# Back up the databases that hold state: accounts (acore_auth), characters (acore_characters) and
# playerbots' tables (acore_playerbots), compressed, into var/backups/db-<time>.sql.zst. Keeps the
# newest $KEEP (default 14). acore_world is not backed up: dbimport rebuilds it from AzerothCore's SQL.
# MySQL must be running; tools/server-stop.sh runs this before it stops MySQL.
#
# Restore (replaces those three databases; stop the world server first):
#   zstd -dc var/backups/db-<time>.sql.zst | mysql --defaults-file=out/server/etc/mysql.cnf -uroot
# Usage: tools/db-backup.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CNF=$ROOT/out/server/etc/mysql.cnf
DIR=$ROOT/var/backups
KEEP=${KEEP:-14}
mysqladmin --defaults-file="$CNF" -uroot ping >/dev/null 2>&1 || {
  echo "mysql is not running (tools/db-start.sh)"; exit 1; }
mkdir -p "$DIR"
OUT=$DIR/db-$(date +%Y%m%d-%H%M%S).sql.zst
trap 'rm -f "$OUT.tmp"' EXIT
mysqldump --defaults-file="$CNF" -uroot --single-transaction --routines --triggers --set-gtid-purged=OFF \
  --databases acore_auth acore_characters acore_playerbots | zstd -q -T0 -o "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
ls -1t "$DIR"/db-*.sql.zst | tail -n +$((KEEP + 1)) | xargs -r rm --
echo "backup: ${OUT#$ROOT/} ($(du -h "$OUT" | cut -f1))"
