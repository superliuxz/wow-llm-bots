#!/usr/bin/env bash
# Link our modules into AzerothCore's modules/ dir (it only builds modules found there). server_module/
# is our one module, linked under the name AzerothCore expects (mod-llm-brain).
# and keep the links out of the submodule's git status.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
AC=$ROOT/third_party/azerothcore-wotlk
link() {  # link <target> <name>
  ln -sfn "$1" "$AC/modules/$2"
  EX=$(git -C "$AC" rev-parse --git-path info/exclude); EX=$(cd "$AC" && realpath -m "$EX")
  mkdir -p "$(dirname "$EX")"; grep -qxF "/modules/$2" "$EX" 2>/dev/null || echo "/modules/$2" >> "$EX"
}
link "$ROOT/third_party/mod-playerbots" mod-playerbots
[ -d "$ROOT/server_module" ] && link "$ROOT/server_module" mod-llm-brain
# Root compile_commands.json for clangd (gitignored).
ln -sfn "$ROOT/out/server/compile_commands.json" "$ROOT/compile_commands.json"
echo "setup done"
