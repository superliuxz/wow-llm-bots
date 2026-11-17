#!/usr/bin/env bash
# Build out/server/etc/*.conf from the .dist templates plus our overrides in config/.
# @ROOT@ in overrides becomes the repo root. The DB password comes from var/secrets/db-password
# (untracked, this user only): state, not something out/ may lose. An older one in
# out/server/etc/.acore-db-password is moved there.
# Usage: tools/apply-config.sh [test-name]   (adds config/tests/<test-name>.*.overrides)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ETC=$ROOT/out/server/etc
SECRETS=$ROOT/var/secrets
if [ ! -f "$SECRETS/db-password" ] && [ -f "$ETC/.acore-db-password" ]; then
  mkdir -p "$SECRETS"; chmod 700 "$SECRETS"
  mv "$ETC/.acore-db-password" "$SECRETS/db-password"; chmod 600 "$SECRETS/db-password"
fi
if [ ! -s "$SECRETS/db-password" ]; then
  echo "no DB password in $SECRETS/db-password: create it (one line, mode 600) first" >&2
  exit 1
fi
PW=$(head -n1 "$SECRETS/db-password")
apply() {  # apply <dist> <out> <overrides...>
  local dist=$1 out=$2; shift 2
  cp "$dist" "$out"; chmod 600 "$out"
  for ov in "$@"; do
    # "|| [ -n ...]": also the last line when the file doesn't end in a newline.
    while IFS= read -r line || [ -n "$line" ]; do
      line=${line%$'\r'}
      [[ -z "${line//[[:space:]]/}" || "$line" =~ ^[[:space:]]*\# ]] && continue
      [[ "$line" == *=* ]] || { echo "ignored (no '='): $line" >&2; continue; }
      key=${line%%=*}; key=${key%"${key##*[![:space:]]}"}
      val=${line#*=}; val=${val#"${val%%[![:space:]]*}"}; val=${val//@DB_PASSWORD@/"$PW"}; val=${val//@ROOT@/"$ROOT"}
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
if [ -f "$ETC/modules/mod-llm-brain.conf.dist" ]; then
  apply "$ETC/modules/mod-llm-brain.conf.dist" "$ETC/modules/mod-llm-brain.conf" "$C/llm-brain.overrides" $(extra llm-brain)
fi
echo "config applied${T:+ (test: $T)}"
