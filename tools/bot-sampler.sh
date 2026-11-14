#!/usr/bin/env bash
# Sample random bots' state from the characters DB every N seconds into JSONL.
# Usage: tools/bot-sampler.sh <out.jsonl> [interval_s] [account LIKE pattern, default RNDBOT%]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1; INT=${2:-60}; ACC=${3:-RNDBOT%}
mkdir -p "$(dirname "$OUT")"
Q="SELECT JSON_OBJECT('t', UNIX_TIMESTAMP(), 'name', c.name, 'race', c.race, 'class', c.class,
  'level', c.level, 'xp', c.xp, 'money', c.money, 'map', c.map, 'zone', c.zone,
  'x', ROUND(c.position_x,1), 'y', ROUND(c.position_y,1), 'z', ROUND(c.position_z,1),
  'online', c.online, 'played', c.totaltime,
  'q_active', (SELECT COUNT(*) FROM acore_characters.character_queststatus q WHERE q.guid=c.guid),
  'q_rewarded', (SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r WHERE r.guid=c.guid),
  'dead', (SELECT COUNT(*) FROM acore_characters.corpse k WHERE k.guid=c.guid))
FROM acore_characters.characters c JOIN acore_auth.account a ON a.id=c.account
WHERE a.username LIKE '$ACC' AND c.online = 1"
while true; do
  mysql --defaults-file="$ROOT/config/mysql.cnf" -uroot -N -B -e "$Q" >> "$OUT" || true
  sleep "$INT"
done
