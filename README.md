# wow-llm-bots

A WoW 3.3.5a (WotLK) private server where LLM-driven bots play like real people:
they level from 1 to 80, quest, chat, form guilds, run dungeons and raid with you.
Everything runs locally (AzerothCore + mod-playerbots, local models on one GPU).

Design and every decision so far: [docs/design.md](docs/design.md).

## Layout

| Path | What |
|---|---|
| `docs/` | design doc |
| `private/` | private submodule (accounts, personal data); not accessible to others |
| `third_party/` | upstream code as pinned git submodules: AzerothCore (mod-playerbots fork, `Playerbot` branch), mod-playerbots (`master`), and harness deps: Abseil `20260817.0`, GoogleTest `v1.18.0`, libevent `2.1.13-stable`, cpp-httplib `v0.66.0`, nlohmann/json `v3.12.0` |
| `server_module/` | our AzerothCore module(s) (AzerothCore style) |
| `harness/` | the bot brain harness (C++) |
| `content/` | shared bot knowledge: `RULES.md`, `concepts/`, prompts |
| `config/` | our config files (MySQL, server overrides) |
| `tools/` | setup, build, data extraction, DB start/stop |
| `out/` | build outputs (not tracked) |
| `server/` | installed server, extracted map data, MySQL data, logs (not tracked) |
| `var/bots/` | live bot memory, its own git repo (not tracked here) |
| `reference/` | downloaded addon sources (not tracked) |

## Setup

```sh
git submodule update --init   # private/ will fail without access; that's fine
tools/setup.sh              # link modules into AzerothCore, clangd compile_commands.json
tools/configure-server.sh   # CMake: clang + lld + Ninja, Percona client
tools/build-server.sh       # build and install into server/
tools/extract-data.sh       # extract maps/vmaps/mmaps/DBC from the 3.3.5a client
tools/db-start.sh           # project-local MySQL (Percona)
```

Code style: Google/Chromium C++ style (`.clang-format`).

License: GPL-2.0 (same as AzerothCore and mod-playerbots), see [LICENSE](LICENSE).

Server deps stay system packages (Arch/CachyOS): `clang lld cmake ninja boost percona-server percona-server-clients`.
