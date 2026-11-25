# wow-llm-bots

A WoW 3.3.5a (WotLK) private server where LLM-driven bots play like real people:
they level from 1 to 80, quest, chat, form groups, run dungeons, and raid with you.
Everything runs locally (AzerothCore + `mod-playerbots`, local models on one GPU).

Design: [docs/design.md](docs/design.md). Open work: [docs/TODO.md](docs/TODO.md). Measured
numbers: [docs/results.md](docs/results.md).

## Layout

| Path | What |
|---|---|
| `CONTRIBUTING.md` | C++ style, checks, pure-harness rule, repo layout (`CLAUDE.md` and `.agents/rules/contributing.md` point here) |
| `docs/` | `design.md`, `TODO.md`, `results.md`, and `specs/` (`snapshot.md`, `triggers.md`, `options.md`, `grammar-idl.md`, `harness.md`) |
| `common/` | shared C++20: frozen `base/` runtime primitives and `net/` Unix socket framing |
| `snapshot/` | C++20 snapshot/command wire structs (`snapshot.h`) and Glaze BEVE/JSON codecs |
| `server_module/` | AzerothCore module (`mod-llm-brain`): snapshots, unified `BotAction` + area-split executors, `BotSession`, combat/soak tracing |
| `harness/` | C++26 brain process (`out/common/harness`): `run`, `decide`, `render-screen`, `replay` |
| `content/` | LLM inputs: `RULES.md`, 54 dungeon/raid `notes.md`, character `identities/` |
| `config/` | MySQL and server `.overrides` rendered by `tools/wlb config` |
| `tools/` | `tools/wlb` CLI (`tools/wlblib/`) plus focused bench/eval/generator scripts |
| `experimental/` | parked research (human-likeness scorer, `notes_hill_climb`, parked design docs); skipped by `wlb check` and CI |
| `third_party/` | pinned upstream submodules (AzerothCore, `mod-playerbots`, Glaze, GoogleTest, cpp-httplib, nlohmann/json, llama.cpp) |
| `out/` | generated builds and `out/server` / `out/server-test` (untracked, safe to delete) |
| `var/` | persistent MySQL data, backups, logs, bot memory (untracked) |
| `models/`, `private/` | local GGUF models and private submodule (untracked) |

## Setup

```sh
git submodule update --init   # private/ will fail without access; that's fine
tools/wlb setup links         # link modules into AzerothCore, clangd compile_commands.json
tools/wlb build common        # shared C++ + harness + unit tests
tools/wlb setup configure     # CMake: clang + lld + Ninja, Percona client
tools/wlb build server        # build and install into out/server
tools/wlb setup data          # extract maps/vmaps/mmaps/DBC from the 3.3.5a client
tools/wlb db start            # project-local MySQL (Percona)
```

## Playing with the bots

`tools/wlb server start` brings up the play server with a party preset (`config/llm-brain.overrides`):
Drogbar (orc warrior, tank), Zenji (troll priest, healer), Rokzul (orc hunter) and Mabaku (troll
mage), plus Gatda (orc rogue, 23) and Testpriest (priest, 31). Missing preset characters are created on the first start
(`LlmBrain.CreateBots`) at the troll/orc starting area and keep their progress afterwards. Make a
level-1 Horde character there, invite them, and quest, queue for Dungeon Finder, or pug in the
LookingForGroup channel. Start the brain with `tools/wlb llm` and `tools/wlb harness run`.

## Tools (`tools/wlb`)

Single Python stdlib CLI (`tools/wlb <command> --help`). Acts only on this checkout's PID files.

| Command | What |
|---|---|
| `tools/wlb check [format\|rules\|includes\|snapshots\|triggers\|goldens\|sanitizers]` | fast pre-commit checks (`--fix` for format/rules/includes; leave sanitizers to CI) |
| `tools/wlb build common [asan\|tsan\|msan\|server]` | build `common/` + `harness` and run unit tests |
| `tools/wlb build server [--test]` | build and install AzerothCore + `mod-llm-brain` (`out/server` or `out/server-test`) |
| `tools/wlb test unit\|e2e\|perf\|eval\|soak [NAME...]` | test suites (`--list` shows all cases; e2e runs in 6 grouped suites on `out/server-test`) |
| `tools/wlb config [test]` | render `out/server/etc/*.conf` from templates + `config/*.overrides` |
| `tools/wlb server start\|stop [--test]` | start/stop MySQL + authserver + worldserver (stop backs up DBs first) |
| `tools/wlb db start\|stop\|backup` | manage project-local Percona MySQL |
| `tools/wlb console [--test] <cmd>` | send a command to the worldserver console |
| `tools/wlb llm` | start `llama-server` with the measured fastest settings |
| `tools/wlb snap capture\|fake` | capture live snapshots to JSON or run the stand-in socket harness (`wlblib/wire.py`) |
| `tools/wlb trace meter\|dodge\|stalls\|mine` | boss DPS/HPS meter, dodge timeline, soak stall detector, failure-case miner |
| `tools/wlb gen invisible-displays\|quest-poi\|knowledge\|boss-notes\|grammar\|fixture-options` | regenerate derived tables, knowledge JSON, boss notes, grammars, or fixture options |
| `tools/wlb setup links\|configure\|data\|llama\|msan\|client-ui` | one-time local environment setup |
| `tools/wlb remote list\|<name> sync\|run\|status\|stop\|pull\|install-hook` | optional remote CPU worker offload (`private/remotes.json`, explicit use only) |

Code style: Google/Chromium C++ style (`.clang-format`). License: GPL-2.0 ([LICENSE](LICENSE)).

Toolchain (`common/` + `harness/`): `clang lld cmake ninja python ccache`.
Server deps (`worldserver`): `boost percona-server percona-server-clients`.
