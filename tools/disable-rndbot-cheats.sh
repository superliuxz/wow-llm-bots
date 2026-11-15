#!/usr/bin/env bash
# Stop playerbots' random-bot manager from "refreshing" bots (free revive + full heal)
# and random-teleporting/re-randomizing them. It fires whenever a bot has no
# "teleport"/"randomize" event on record (RandomPlayerbotMgr.cpp ~1715-1769), so we
# pre-seed both events for every random-bot character, valid for 10 years.
# Bots keep the human ways out: corpse run, spirit healer, hearthstone.
# Run while worldserver is STOPPED (the manager caches events in memory at startup).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if pgrep -x worldserver >/dev/null; then echo "stop worldserver first"; exit 1; fi
"$ROOT/tools/db-start.sh" >/dev/null
mysql --defaults-file="$ROOT/server/etc/mysql.cnf" -uroot <<'SQL'
DELETE FROM acore_playerbots.playerbots_random_bots WHERE event IN ('teleport','randomize');
INSERT INTO acore_playerbots.playerbots_random_bots (owner, bot, time, validIn, event, value)
SELECT 0, c.guid, UNIX_TIMESTAMP(), 315360000, e.event, 1
FROM acore_characters.characters c
JOIN acore_auth.account a ON a.id = c.account
CROSS JOIN (SELECT 'teleport' AS event UNION ALL SELECT 'randomize') e
WHERE a.username LIKE 'RNDBOT%';
SELECT event, COUNT(*) AS bots, MIN(validIn) AS valid_s FROM acore_playerbots.playerbots_random_bots
WHERE event IN ('teleport','randomize') GROUP BY event;
SQL
