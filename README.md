# wow-llm-bots

A WoW 3.3.5a (WotLK) private server where LLM-driven bots play like real people:
they level from 1 to 80, quest, chat, form guilds, run dungeons and raid with you.
Everything runs locally (AzerothCore + mod-playerbots, local models on one GPU).

Design and every decision so far: [docs/design.md](docs/design.md).

## Layout

| Path | What |
|---|---|
| `CONTRIBUTING.md` | the rules for working in this repo (C++ style and checks, layout); `CLAUDE.md` only imports it, for Claude Code |
| `docs/` | the design (`design.md`), plans, specs and measured results; **`TODO.md` is the list of open work** |
| `private/` | private submodule (accounts, personal data); not accessible to others |
| `third_party/` | upstream code as pinned git submodules: AzerothCore (mod-playerbots fork, `Playerbot` branch), mod-playerbots (`master`), harness/common deps: Glaze `v9.1.0`, GoogleTest `v1.18.0`, cpp-httplib `v0.66.0`, nlohmann/json `v3.12.0`, and llama.cpp `v1.0.0` |
| `common/` | shared C++ used by the server module and the harness: `base/` (Chromium-style threads, epoll IO message pump, task runners, callbacks, WeakPtr, scoped_refptr, TimeDelta, strings), `net/` (Unix socket server/client, length-prefixed framing), and `proto/` (plain C++20 snapshot/command structs + Glaze BEVE/JSON wire codecs) |
| `server_module/` | our AzerothCore module: sees the game, builds each bot's snapshot, carries out commands |
| `harness/` | the brain's process (C++), separate from the game server: screens, plans, the llama client, prompts, the decision log, the scheduler, and `harness decide` to run one decision offline (`docs/plans/harness.md`); built as `out/common/harness` |
| `content/` | shared bot knowledge: `RULES.md` (the rules every bot starts from); `concepts/` and prompts are planned |
| `config/` | our config files (MySQL, server overrides) |
| `tools/` | `tools/wlb`, the one command-line tool, and research scripts: see [Tools](#tools) |
| `out/` | everything generated, as `out/<name>` for what builds it (not tracked). **Safe to delete: every part comes back from a command.** `out/server` is the installed game server (binaries, configs, the extracted map data, sockets); rebuild it with `tools/wlb build server`, `tools/wlb config` and `tools/wlb setup data` |
| `var/` | **state worth keeping, not tracked.** `backups/` (database dumps, one per server stop), `mysql-data/` (the database: accounts, characters, bot progress), `logs/` (server logs and the samples of your own play), and, once the harness exists, `bots/` (live bot memory: `<name>/identity.md`, goals, `events/*.jsonl`; its own git repo) |
| `models/` | the GGUF model files llama-server loads (not tracked) |
| `reference/` | downloaded addon sources (not tracked) |

## Setup

```sh
git submodule update --init   # private/ will fail without access; that's fine
tools/wlb setup links         # link modules into AzerothCore, clangd compile_commands.json
tools/wlb build common        # shared C++ (common/) + unit tests
tools/wlb setup configure     # CMake: clang + lld + Ninja, Percona client
tools/wlb build server        # build and install into out/server
tools/wlb setup data          # extract maps/vmaps/mmaps/DBC from the 3.3.5a client
tools/wlb db start            # project-local MySQL (Percona)
```

## Tools

One command-line tool, `tools/wlb` (Python standard library only; modules in `tools/wlblib/`).
Run it from anywhere: it finds the repo from its own location. `tools/wlb <command> --help` says
more. It acts on this checkout's processes only (by their pid files), never by name.

**Daily**

| Command | What |
|---|---|
| `tools/wlb check` | fast everyday checks before review or commit: format, trigger table, default build + unit tests, snapshot examples, includes, screen goldens |
| `tools/wlb check sanitizers` | build and run unit tests under ASan+UBSan, TSan, and MSan (`--sanitizers` / `--san` on `wlb check` runs everything) |
| `tools/wlb check format\|includes\|snapshots\|triggers\|goldens\|sanitizers` | one of those (`--fix` for format and includes) |
| `tools/wlb build common [asan\|tsan\|msan\|server]` | build `common/` (and the harness) and run the unit tests; `server` is the copy linked into AzerothCore |
| `tools/wlb build server [--test]` | build and install AzerothCore with our module (`--test` adds the test-only fixtures) |
| `tools/wlb test unit\|e2e\|perf\|eval [NAME...]` | the four test targets: `unit` (gtest, `--filter`, `--variant`), `e2e` (live scenarios on a test build of the server; stops the play server and restores it), `perf` (measurements checked against `tools/wlblib/cases/perf_baselines.json` and hard limits; `--update-baselines` records a run, `--capture` records the brain replay's input), `eval`; `--list` shows the cases |
| `tools/wlb config [test]` | render `out/server/etc/*.conf` from the templates plus `config/*.overrides` (and `config/tests/<test>.*`) |
| `tools/wlb server start\|stop` | MySQL, authserver and worldserver together (stop backs the databases up first) |
| `tools/wlb db start\|stop\|backup` | the project-local MySQL alone; `backup` dumps accounts, characters and playerbots' tables to `var/backups/` (compressed, newest 14 kept; the restore command is in `tools/wlblib/server.py`) |
| `tools/wlb console <command>` | send a command to the worldserver console |
| `tools/wlb llm` | llama-server with the settings measured fastest (`docs/phase2-results.md`) |

**Once**

| Command | What |
|---|---|
| `tools/wlb setup links` | link our modules into AzerothCore, `compile_commands.json` for clangd |
| `tools/wlb setup configure` | CMake for the server (clang, lld, Ninja, Percona client) |
| `tools/wlb setup data` | maps, vmaps, mmaps and DBC from the 3.3.5a client |
| `tools/wlb setup llama` | llama.cpp with CUDA |
| `tools/wlb setup msan` | MSan-instrumented libc++ (built by `tools/wlb build common msan` when missing) |
| `tools/wlb setup client-ui` | the client's UI code and event names, for the client-event audit |

**Snapshot and brain research** (Python, dev only; production is C++)

| Path | What |
|---|---|
| `tools/snapshot/` | `wire_py.py` (`ctypes` wrapper for `libwlb_wire_py.so`), `fake_harness.py` (a stand-in harness that prints what the module sends) |
| `tools/audit/` | the client-event audit: the scripts `tools/wlb setup client-ui` feeds (`docs/specs/coverage-client-events.md`); `gen_invisible_displays.py` regenerates the table of invisible creature models (`server_module/src/snapshot/invisible_displays.h`) |
| `tools/bench/` | brain load benches: `capture_snapshots.py` (live snapshots to files), `trigger_sim.py` (how often bots wake), `brain_load.py` (wake-ups replayed against llama-server), `throughput.py` (capacity), `prompt_render.py` (the compact prompt) |


Code style: Google/Chromium C++ style (`.clang-format`).

License: GPL-2.0 (same as AzerothCore and mod-playerbots), see [LICENSE](LICENSE).

System toolchain (`common/` + `harness/`): `clang lld cmake ninja python ccache`.
Upstream AzerothCore server deps (`worldserver` only): `boost percona-server percona-server-clients`.
