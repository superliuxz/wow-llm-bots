#!/usr/bin/env bash
# Start the project-local MySQL (Percona) instance in the background.
set -euo pipefail
S=/mnt/storage/wow-llm-bots/server
if mysqladmin --defaults-file=/mnt/storage/wow-llm-bots/config/mysql.cnf -uroot ping >/dev/null 2>&1; then echo "mysql already running"; exit 0; fi
mysqld --defaults-file=/mnt/storage/wow-llm-bots/config/mysql.cnf --daemonize
for i in $(seq 1 30); do
  mysqladmin --defaults-file=/mnt/storage/wow-llm-bots/config/mysql.cnf -uroot ping >/dev/null 2>&1 && { echo "mysql up"; exit 0; }
  sleep 1
done
echo "mysql failed to start; see $S/logs/mysql.log"; exit 1
