# Options, Unified Actions, and Panels

The server module turns live world state into a numbered list of mechanically valid **options**
(what a real player could click or do right now). The harness never numbers options itself — it
renders `snapshot.options` verbatim and sends back `Command::Choose(seq, option)`.

## Design Rules

1. **Facts only, no C++ gameplay judgments.** `AddOptions` (`server_module/src/options/options.cc`)
   exposes what is mechanically available (nearby hostile/neutral mobs, lootable corpses,
   gatherable nodes matching learned skill, usable quest/world objects, NPC services, bag
   consumables/gear with factual ilvl/stat deltas, talent trees with point counts, party/LFG/roll/
   ready-check/rez popups, travel spots and hubs, and `Keep doing what I'm doing`). Deciding which
   option to pick belongs to the LLM brain.
2. **One code path, live and offline.** `AddOptions(Snapshot*, const OptionFacts&)` is pure C++
   with zero AzerothCore dependencies. Live `SnapshotBuilder` populates `OptionFacts` from world
   state; offline `fixture_options` reads `<case>.facts.json` and runs the exact same function.
3. **One typed action variant (`BotAction`).** Every chosen option is decoded **once** at option
   creation into a typed `std::variant` (`BotAction` in `server_module/src/options/action_types.h`)
   covering both `Command::Choose` options and `Command::Act` standing orders (`StandingActAction`).
   Executors and `LlmBrainWorldScript` dispatch via `std::visit` / `std::get_if` with no `substr`
   option-ID parsing.
4. **One validate-before-cancel execution path.** Both `OnChoose` and `OnAct` validate via
   `ValidateAction` and run via `ExecuteAction` (`server_module/src/options/option_executor.cc`):
   - An expired, dead-blocked, or combat-blocked choice or invalid act fails cleanly with an
     `ErrorEvent` **without** cancelling an active boss dodge (`ActiveAct`).
   - Shared rules (raid marking: anyone in 5-man party, leader/assistant in raid; dual-slot item
     equipping for rings/trinkets/dual-wield; safe movement cancellation checking flight/CC/vehicle)
     live once in `exec_shared.cc` so options and standing-order acts never diverge.
5. **Opcode parity.** Loot, quest accept/turn-in, shared quest accept/push, loot roll, ready check,
   group invite/accept/decline/leave, release spirit, innkeeper bind, cancel aura, and item/spell
   use route through AzerothCore's real `WorldSession` opcode handlers or equivalent server checks.

## Area-Split Executors (`server_module/src/options/`)

| File | Responsibility |
|---|---|
| `action_types.{h,cc}` | Typed `BotAction` variant, fact-GUID decoder, lifecycle predicates (`IsDeadAllowedAction`, `IsPendingCastAction`, `IsGameObjectAction`). |
| `options.{h,cc}` | Pure `AddOptions` + `OptionHistory` (30 s window for resolving `(seq, option)`). |
| `option_executor.{h,cc}` | Unified `ValidateAction` + `ExecuteAction` dispatcher (`std::visit`) and `ExecuteAttackAction`. |
| `exec_shared.{h,cc}` | GUID parsing, navmesh `WalkTo`/`WalkNextTo`, `CanClearBotMovement`, `MarkUnitWithIcon`, `PickEquipSlot`/`EquipBagItem`, cast tracking, `UseObjectNow`. |
| `exec_npc.{h,cc}` | `CollectNpcActions` (without mutating `PlayerTalkClass`), `TalkToNpcNow`, quest accept/turn-in, class training, vendor sell-grey/repair/buy, flight paths, hearthstone bind. |
| `exec_items.{h,cc}` | Bag food/drink & rest tracking, usable/equippable bag items, talent tree discovery & spending, corpse looting (`HasTakeableLoot`, `LootCorpseNow`), object use. |
| `exec_social.{h,cc}` | Group invites/leave, loot rolls, ready checks, shared quests, LFG queue/role/proposal/teleport, group policies (`not_tank`, `stay`), follow leader, raid mark, mount, rez. |
| `exec_movement.{h,cc}` | Multi-step mmap `TickLongWalk` (~70 yd steps, stuck/ping-pong detector, validated dungeon seam hops). |
| `act_executor.{h,cc}` | Standing-order combat movement/targeting/casting (`to`, `away`, `out of`, `back`, `kite`, `use`, `stop`, `avoid`, `face`, `jump`, `cancel`, `interact`, `board`, `leave`, `pet`). |

## Open Panels & Hover

`Command::Open` / `Close` / `Hover` update `UiState` (`server_module/src/snapshot/ui_state.{h,cc}`),
causing the next snapshot to populate `ext.bags`, `ext.map`, `ext.character`, `ext.quest_log`,
`ext.spellbook`, or `base.tooltips`. `BotLoop` caches remembered panel contents across wakes and
passes them into `AssemblePromptInput`.
