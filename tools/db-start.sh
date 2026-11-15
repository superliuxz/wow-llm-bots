#!/usr/bin/env bash
# Start the project-local MySQL (Percona) instance in the background.
# Renders config/mysql.cnf.in to server/etc/mysql.cnf first.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
S=$ROOT/server
CNF=$S/etc/mysql.cnf
mkdir -p "$S/etc"
sed "s|@ROOT@|$ROOT|g" "$ROOT/config/mysql.cnf.in" > "$CNF"
if mysqladmin --defaults-file="$CNF" -uroot ping >/dev/null 2>&1; then echo "mysql already running"; exit 0; fi
mysqld --defaults-file="$CNF" --daemonize
for i in $(seq 1 30); do
  mysqladmin --defaults-file="$CNF" -uroot ping >/dev/null 2>&1 && { echo "mysql up"; exit 0; }
  sleep 1
done
echo "mysql failed to start; see $S/logs/mysql.log"; exit 1
