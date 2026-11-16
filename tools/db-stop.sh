#!/usr/bin/env bash
# Stop the project-local MySQL instance.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mysqladmin --defaults-file="$ROOT/out/server/etc/mysql.cnf" -uroot shutdown && echo "mysql stopped"
