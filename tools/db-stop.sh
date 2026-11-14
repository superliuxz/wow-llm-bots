#!/usr/bin/env bash
# Stop the project-local MySQL instance.
set -euo pipefail
S=/mnt/storage/wow-llm-bots/server
mysqladmin --defaults-file=/mnt/storage/wow-llm-bots/config/mysql.cnf -uroot shutdown && echo "mysql stopped"
