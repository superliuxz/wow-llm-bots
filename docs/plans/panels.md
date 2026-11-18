# Plan: open panels (bags, character, quest log) and hover (2026-11-18, agreed; built)

The last part of #4 (TODO "Next" #1). The spec already fixes the shape (`docs/specs/snapshot.md`
§3, §5, §9; decisions 10 and 12): panels are `visibility: OPEN`, listed in
`base.ui.open_panels`, opened and closed with the `open` / `close` commands, and routine looking
is done by per-bot habits in the harness, which also remembers closed panels with their age. This
proposal is about what to build first and how the brain gets a panel open.

## Module

- **State:** each bot has its set of open panels (`BotState`), sent as `base.ui.open_panels`.
  `Command.open` / `Command.close` add or remove one; nothing goes to the game (the client opens
  bags and the character sheet without a packet). Reset on harness connect, like the rest.
- **Fill, only while open** (the validator already rejects them otherwise):
  - `ext.bags`: every item (bag and slot, count, quality, kind, usable, cooldown, sell price),
    free / total slots, hearthstone (bound place, cooldown), keyring.
  - `ext.character`: equipped items (slot, durability, item level), the character sheet's stats;
    GearScore stays empty (an addon).
  - `ext.quest_log`: every quest with its objectives (the `quest_tracker` code), description,
    rewards and reward choices, money, shareable.
- **Auto-open with windows** (a merchant opens bags) waits for the `interaction` extension: Talk
  does its work in one go today, so no window stays open.
- Cost: bags and quest log are a few hundred items at most and are built only while open; a
  `perf snapshots` run with all three open measures it.

## Harness

- **Rendering:** compact lines under their own headings, grouped the way the client shows them,
  e.g. `bags (3 of 16 free): Tough Hunk of Bread x4 (food), Refreshing Spring Water x2 (drink),
  Chunk of Boar Meat x3 (gray, sells 1c each) ...`. No caps (spec §7: the harness decides what is
  noticed); measured in prompt tokens.
- **Remembered views:** when a panel closes, the harness keeps its last contents and renders them
  as memory with their age (`bags, seen 40 s ago: ...`), never as live state.
- **How a panel gets opened:** see question 1.

## Hover (items and quests first)

- **Command:** `hover {kind, id}` (already on the wire: `Command.hover`, `HoverRef`); the tooltip is
  in `base.tooltips` for the next few snapshots (`visibility: HOVER`), then gone, as in the
  client. Like `open`, the brain asks with a reply line (`hover: <thing>`) and is woken again once
  the tooltip shows; habits hover too (new loot, a quest reward choice).
- **Item tooltips:** from the item template: name and quality, binding, slot and type, armor and
  damage, stats, durability, required level and class, sell price, set; "is it an upgrade" from
  playerbots' stat weights (`StatsWeightCalculator`, already used for quest rewards).
  "Use:" / "Equip:" / "Chance on hit:" lines come from spell descriptions, whose `$s1`, `$d`,
  `$o1`, `$t1` ... tokens the client fills in: a small formatter for the common tokens, the
  rest left as written.
- **Quest tooltips:** title, level, objectives with progress, rewards.
- **Later:** spells and auras (with the spellbook), units, places.

## As built (2026-11-18)

- Module: `snapshot/ui_state.{h,cc}` (open panels, hover for 5 snapshots), `snapshot/panels_builder`
  (bags, character, quest log, item and quest tooltips), `snapshot/spell_text` (token formatter and
  the Spell.dbc description reader: AzerothCore doesn't load descriptions, so the module reads
  field 170 of `DataDir/dbc/Spell.dbc` at startup for the spells items cast). Commands
  `open` / `close` / `hover`; a refusal is a non-step `error` event.
- Item fields marked `visibility: HOVER` (item level, binding, sell price, stats) appear only in
  the tooltip, so the bags line shows no sell price until hovered.
- Harness: `open:` / `close:` / `hover:` reply lines (grammar: the three panels; hover names from
  what is on screen: items in open panels, quests in the tracker and log); the brain is woken
  ("looked at bags") when a snapshot shows what it asked for, within 3 s
  (`BotLoopConfig::look_wait`). Closed panels are remembered with their age ("You remember").
- Habits (`BotLoopConfig::habits`), from the snapshot because the module sends no quest or
  inventory-full events yet: bags glanced at when `bag_free_slots` reaches 0, the quest log
  glanced at when the tracker changes (opened, then closed once seen: it is remembered), all
  panels closed when combat starts. "Bags at a vendor" waits for vendor windows.
- `upgrade` compares with the worn item through playerbots' stat weights.

## Tests

Unit: module-independent fill helpers where possible, the renderer (goldens), the grammar line,
remembered views. e2e: `open bags` shows the fixture character's items, `close` removes them.
Eval: the use-item quest and the "food in open bags" scenes of spec §10 become cases.

## Decided (2026-11-18)

1. **Who opens panels:** both. The brain with an optional `open:` reply line (woken again once the
   panel shows); habits in the harness with no LLM call: bags at a vendor or on "inventory is
   full", the quest log for one snapshot after a quest is accepted or completed, everything
   closed when combat starts; parameters the brain can change later.
2. **Remembered views:** now.
3. **Scope:** bags, character and quest log; the other 13 panels later, when something on screen
   needs them (talents at level 10, the map for zone travel).
4. **Hover:** now, for items and quests; spells, auras, units and places later.
