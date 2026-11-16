# wow-llm-bots

A WoW 3.3.5a (WotLK) private server where LLM-driven bots play like real people:
they level from 1 to 80, quest, chat, form guilds, run dungeons and raid with you.
Everything runs locally (AzerothCore + mod-playerbots, local models on one GPU).

Design and every decision so far: [docs/design.md](docs/design.md).

## Layout

| Path | What |
|---|---|
| `docs/` | the design (`design.md`), plans, specs and measured results |
| `private/` | private submodule (accounts, personal data); not accessible to others |
| `third_party/` | upstream code as pinned git submodules: AzerothCore (mod-playerbots fork, `Playerbot` branch), mod-playerbots (`master`), harness deps: Abseil `20260817.0`, GoogleTest `v1.18.0`, cpp-httplib `v0.66.0`, nlohmann/json `v3.12.0`, and llama.cpp `v1.0.0` |
| `common/` | shared C++ used by the server module and the harness: `base/` (Chromium-style threads, epoll IO message pump, task runners, callbacks, WeakPtr, scoped_refptr, TimeDelta) and `net/` (Unix socket server/client, length-prefixed framing) |
| `server_module/` | our AzerothCore module: sees the game, builds each bot's snapshot, carries out commands |
| `harness/` | the brain's process (C++), separate from the game server: screens, plans, and soon the llama client and scheduler (`docs/plans/harness.md`) |
| `content/` | *planned, not created yet:* shared bot knowledge (`RULES.md`, `concepts/`, prompts) |
| `config/` | our config files (MySQL, server overrides) |
| `tools/` | scripts: see [Tools](#tools) |
| `out/` | everything generated, laid out as `out/<name>` for what builds it (not tracked). **`out/server` is the installed game server and holds its state:** binaries, configs, the 7 GB of extracted map data and the MySQL database (accounts, characters). Don't `rm -rf out`; clean specific build directories |
| `var/bots/` | *planned, not created yet:* live bot memory (`<name>/identity.md`, goals, `events/*.jsonl`), its own git repo, written by the harness (not tracked here) |
| `models/` | the GGUF model files llama-server loads (not tracked) |
| `reference/` | downloaded addon sources (not tracked) |

## Setup

```sh
git submodule update --init   # private/ will fail without access; that's fine
tools/setup.sh              # link modules into AzerothCore, clangd compile_commands.json
tools/build-common.sh       # shared C++ (common/) + unit tests; --asan / --tsan / --msan
tools/setup/configure-server.sh   # CMake: clang + lld + Ninja, Percona client
tools/build-server.sh       # build and install into server/
tools/setup/extract-data.sh       # extract maps/vmaps/mmaps/DBC from the 3.3.5a client
tools/db-start.sh           # project-local MySQL (Percona)
```

## Tools

Run from the repo root. Every script finds the repo from its own location.

**Daily**

| Script | What |
|---|---|
| `tools/check-cpp.sh` | everything our C++ must pass before review or commit: format, includes, build and tests in default, ASan+UBSan, TSan and MSan |
| `tools/check-includes.sh` | include-what-you-use check (`--fix` to apply) |
| `tools/build-common.sh` | build `common/` and run its tests (`--asan` / `--tsan` / `--msan`, `--server` for the copy linked into AzerothCore) |
| `tools/build-server.sh` | build and install AzerothCore with our module (`--test` adds the test-only fixtures) |
| `tools/apply-config.sh [test]` | render `out/server/etc/*.conf` from the templates plus `config/*.overrides` (and `config/tests/<test>.*`) |
| `tools/server-start.sh` / `server-stop.sh` | MySQL, authserver and worldserver together |
| `tools/db-start.sh` / `db-stop.sh` | the project-local MySQL alone |
| `tools/wsc <command>` | send a command to the worldserver console |
| `tools/llm-server.sh` | llama-server with the settings measured fastest (`docs/phase2-results.md`) |
| `tools/validate-snapshots.sh` | compile the snapshot proto, validate the example snapshots, check the client-event table |
| `tools/bot-sampler.sh` | sample bot state from the DB to JSONL (started by `server-start.sh`) |

**Once** (`tools/setup/`, plus `tools/setup.sh` and `tools/build-protoc.sh`)

| Script | What |
|---|---|
| `tools/setup.sh` | link our modules into AzerothCore, `compile_commands.json` for clangd |
| `tools/setup/configure-server.sh` | CMake for the server (clang, lld, Ninja, Percona client) |
| `tools/setup/extract-data.sh` | maps, vmaps, mmaps and DBC from the 3.3.5a client |
| `tools/setup/build-llama.sh` | llama.cpp with CUDA |
| `tools/setup/build-msan-libcxx.sh` | MSan-instrumented libc++ (built by `build-common.sh --msan` when missing) |
| `tools/build-protoc.sh` | protoc, built from `third_party/` |

**Snapshot and brain research** (Python, dev only; production is C++)

| Path | What |
|---|---|
| `tools/snapshot/` | `validate_snapshot.py` (used by `validate-snapshots.sh`), `fake_harness.py` (a stand-in harness that prints what the module sends) |
| `tools/audit/` | the client-event audit: `extract-client-ui.sh` and the scripts it feeds (`docs/specs/coverage-client-events.md`); `gen_invisible_displays.py` regenerates the table of invisible creature models (`src/invisible_displays.h`) |
| `tools/bench/` | brain load benches: `capture_snapshots.py` (live snapshots to files), `trigger_sim.py` (how often bots wake), `brain_load.py` (wake-ups replayed against llama-server), `throughput.py` (capacity), `prompt_render.py` (the compact prompt) |


Code style: Google/Chromium C++ style (`.clang-format`).

License: GPL-2.0 (same as AzerothCore and mod-playerbots), see [LICENSE](LICENSE).

Server deps stay system packages (Arch/CachyOS): `clang lld cmake ninja boost percona-server percona-server-clients`.
