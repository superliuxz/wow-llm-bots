#!/usr/bin/env bash
# Extract the 3.3.5a client's default UI code and event names, for the client-event audit
# (docs/specs/coverage-client-events.md). Output is Blizzard's code: untracked, in out/client-ui.
#   out/client-ui/Interface/...     FrameXML, GlueXML, Blizzard_* addons
#   out/client-ui/events_exe.txt    event names in Wow.exe's event tables
# Needs a server build (for libmpq). Client: $WOW_CLIENT (default: WoW-3.3.5a next to the repo).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CLIENT=${WOW_CLIENT:-$(dirname "$ROOT")/WoW-3.3.5a}
OUT=$ROOT/out/client-ui
LIBMPQ=$ROOT/third_party/azerothcore-wotlk/deps/libmpq
mkdir -p "$OUT"
clang -O1 -w -I"$LIBMPQ/libmpq" -I"$LIBMPQ" -I"$ROOT/out/server/deps/libmpq" \
  "$ROOT/tools/audit/mpq_extract.c" "$ROOT/out/server/deps/libmpq/libmpq.a" -lz -lbz2 \
  -o "$OUT/mpq_extract"
rm -rf "$OUT/Interface"
D=$CLIENT/Data; L=$D/enUS
"$OUT/mpq_extract" "$OUT" "$L/patch-enUS-3.MPQ" "$L/patch-enUS-2.MPQ" "$L/patch-enUS.MPQ" \
  "$D/patch-3.MPQ" "$D/patch-2.MPQ" "$D/patch.MPQ" "$L/lichking-locale-enUS.MPQ" \
  "$L/expansion-locale-enUS.MPQ" "$L/locale-enUS.MPQ" "$D/lichking.MPQ" "$D/expansion.MPQ" \
  "$D/common-2.MPQ" "$D/common.MPQ" | tail -1
# Event names: Wow.exe keeps them in a few dense tables of NAME_LIKE_THIS strings. Find the
# tables from the names the UI is known to use, then take every name inside them.
# Chat events are registered from ChatTypeGroup ("CHAT_MSG_"..type), so take every quoted
# CHAT_MSG_ name too.
grep -rhoE 'RegisterEvent\("[A-Z_0-9]+"\)|event *== *"[A-Z_0-9]+"|"CHAT_MSG_[A-Z_0-9]+"' \
  "$OUT/Interface" | grep -oE '"[A-Z_0-9]+"' | tr -d '"' | sort -u > "$OUT/events_ui.txt"
strings -t d -n 4 "$CLIENT/Wow.exe" | awk '$2 ~ /^[A-Z][A-Z0-9]*(_[A-Z0-9]+)+$/ {print $1, $2}' |
  awk 'NR==FNR {k[$1]=1; next} {print $2, ($2 in k)}' "$OUT/events_ui.txt" - |
  awk '{n[NR]=$1; k[NR]=$2} END {
    for (i=1; i<=NR; i++) if (k[i]) {
      if (last && i-last > 12) { if (cnt >= 8) for (j=start; j<=last; j++) print n[j]; start=0; cnt=0 }
      if (!start) start=i; last=i; cnt++
    }
    if (cnt >= 8) for (j=start; j<=last; j++) print n[j]
  }' | sort -u > "$OUT/events_exe.txt"
sort -u "$OUT/events_ui.txt" "$OUT/events_exe.txt" > "$OUT/events_all.txt"
echo "events: $(wc -l < "$OUT/events_ui.txt") used by the UI, $(wc -l < "$OUT/events_exe.txt")" \
  "in Wow.exe's tables, $(wc -l < "$OUT/events_all.txt") together"
