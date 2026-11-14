#!/usr/bin/env bash
# Build server/etc/*.conf from the .dist templates plus our overrides in config/.
# The DB password comes from server/etc/.acore-db-password (untracked).
# Usage: tools/apply-config.sh [test-name]   (adds config/tests/<test-name>.*.overrides)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ETC=$ROOT/server/etc
PW=$(cat "$ETC/.acore-db-password")
apply() {  # apply <dist> <out> <overrides...>
  local dist=$1 out=$2; shift 2
  cp "$dist" "$out"; chmod 600 "$out"
  for ov in "$@"; do
    while IFS= read -r line; do
      [[ -z "$line" || "$line" == \#* ]] && continue
      key=${line%%=*}; key=${key%"${key##*[![:space:]]}"}
      val=${line#*=}; val=${val#"${val%%[![:space:]]*}"}; val=${val//@DB_PASSWORD@/$PW}
      if grep -qE "^${key//./\\.}[[:space:]]*=" "$out"; then
        KEY="$key" VAL="$val" python3 -I -c '
import os,re,sys
p=sys.argv[1]; k=os.environ["KEY"]; v=os.environ["VAL"]
s=open(p).read()
s=re.sub(r"^"+re.escape(k)+r"[ \t]*=.*$", lambda m: k+" = "+v, s, count=1, flags=re.M)
open(p,"w").write(s)' "$out"
      else
        printf '%s = %s\n' "$key" "$val" >> "$out"   # key not in the template: append it
      fi
    done < "$ov"
  done
}
C=$ROOT/config
T=${1:-}
extra() { [ -n "$T" ] && [ -f "$C/tests/$T.$1.overrides" ] && echo "$C/tests/$T.$1.overrides" || true; }
apply "$ETC/worldserver.conf.dist" "$ETC/worldserver.conf" "$C/common.overrides" "$C/worldserver.overrides" $(extra worldserver)
apply "$ETC/authserver.conf.dist"  "$ETC/authserver.conf"  "$C/common.overrides"
apply "$ETC/dbimport.conf.dist"    "$ETC/dbimport.conf"    "$C/common.overrides"
apply "$ETC/modules/playerbots.conf.dist" "$ETC/modules/playerbots.conf" "$C/playerbots.overrides" $(extra playerbots)
echo "config applied${T:+ (test: $T)}"
