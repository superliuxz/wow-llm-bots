#!/usr/bin/env bash
# Extract DBC, maps, vmaps and mmaps from the 3.3.5a client into server/data.
set -euo pipefail

CLIENT=/mnt/storage/WoW-3.3.5a
BIN=/mnt/storage/wow-llm-bots/server/bin
OUT=/mnt/storage/wow-llm-bots/server/data

mkdir -p "$OUT"
cd "$OUT"

echo "== [1/4] dbc + maps  $(date +%T)"
"$BIN/map_extractor" -i "$CLIENT" -o "$OUT"

echo "== [2/4] vmap extract  $(date +%T)"
"$BIN/vmap4_extractor" -d "$CLIENT/Data"

echo "== [3/4] vmap assemble  $(date +%T)"
mkdir -p vmaps
"$BIN/vmap4_assembler" Buildings vmaps

echo "== [4/4] mmaps (slow)  $(date +%T)"
mkdir -p mmaps
"$BIN/mmaps_generator" --config "$BIN/mmaps-config.yaml" --threads "$(nproc)"

echo "== done  $(date +%T)"
du -sh dbc maps vmaps mmaps
