#!/usr/bin/env bash
# Configure the AzerothCore build in out/server (clang + lld + Ninja, Percona client).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cmake -S "$ROOT/third_party/azerothcore-wotlk" -B "$ROOT/out/server" -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$ROOT/out/server" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DAPPS_BUILD=all -DTOOLS_BUILD=all -DMODULES=static -DSCRIPTS=static \
  -DMYSQL_CONFIG=/usr/bin/perconaserver_config -DMYSQL_INCLUDE_DIR=/usr/include/perconaserver \
  -DMYSQL_LIBRARY=/usr/lib/libperconaserverclient.so -DMYSQL_ADD_INCLUDE_PATH=/usr/include/perconaserver
