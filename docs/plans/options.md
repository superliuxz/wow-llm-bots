# Plan: Talk, Rest, Explore, Go to (2026-11-17, built)

The four options the 13 `planned` eval cases need (TODO "Next" #1). For each: what it targets,
when it is offered, and what carries it out. Like the existing options, the module offers facts
(what a player could click or walk to); the brain judges which one.

## Talk to <name>

- **Targets:** a friendly NPC within nameplate range that has a quest marker (available or
  turn-in) or an interactive NPC flag (`UNIT_NPC_FLAG_TRAINER`, `UNIT_NPC_FLAG_TRAINER_CLASS`,
  `UNIT_NPC_FLAG_VENDOR`, `UNIT_NPC_FLAG_REPAIR`, `UNIT_NPC_FLAG_INNKEEPER`,
  `UNIT_NPC_FLAG_FLIGHTMASTER`). One option per name, nearest first. Trainers of another class
  are offered too; a player can click them. On screen, `Unit::subtitle` (`"Priest Trainer"`,
  `"Mage Trainer"`, `"Innkeeper"`, etc. — nameplate-visible; `Unit::services` stays hover-only
  per `snapshot.md` §3) tells the brain whose trainer it is (`priest_picks_own_trainer` tests
  that).
- **Which NPC flags:** read from `OptionFacts` (see "One code path, live and offline" below),
  not guessed from the subtitle.
- **When:** alive, not in combat.
- **Carried out:** walk next to it (`WalkNextTo`), then on arrival do the thing the NPC is there
  for, by calling playerbots' actions directly (its gossip only works for a bot's master):
  - quest giver: turn in finished quests (playerbots' `BestRewardIndex` picks the reward), accept
    the ones on offer (`AcceptQuest` / `TurnInQuest` in `NewRpgBaseAction`);
  - trainer: learn what the bot can afford;
  - vendor or repair: sell grey items, repair.
  Done by `done_command_id` when the module has finished the talk on arrival, with an `info`
  summary line (or the natural `quest_accepted` / `quest_complete` / `money_changed` events) so
  the brain sees what happened and does not immediately re-pick `Talk to <name>`.
- **Not in Talk:** binding the hearthstone at an innkeeper changes where home is, a judgment:
  a later option of its own (through the bind spell, not `SetHomebind`). Training goes through
  the server's `Trainer::TeachSpell`.
- **Later:** open the window (`ext.interaction`) and let the brain pick each quest, reward,
  spell and item. "Accept all, learn everything affordable, sell greys" is the neutral default
  until that panel exists; note that learning everything can spend all the bot's money.

## Rest

- **Targets:** none (the bot itself).
- **When:** alive, out of combat, not mounted or swimming, and health or mana below full. No
  threshold: whether 90% is worth resting at is the brain's call (so it is on screen after almost
  every fight). The text says what the bot
  has in its bags: `Rest (eat and drink)`, `Rest (eat)`, `Rest (drink)`, or `Rest (sit, no food)`
  (food and drink in bags come from `OptionFacts`, not guessed from the power type).
- **Carried out:** playerbots' `EatAction` / `DrinkAction` (best food and water in the bags), or
  sitting when there is none. While the step runs, the module starts the other of eat / drink
  once the first is going (`ContinueRest`, once a second: a second item used in the same tick is
  refused), and holds the body's AI while it sits without food (playerbots stands a sitting bot
  up at its next check). `Playerbots.BotCheats` stays empty (no free food). Tracked in
  `OptionExecutor` as a pending action and marked `done_command_id` when health and mana reach
  full (or the food/drink aura expires, or combat starts).

## Explore <direction>

- **Targets:** up to 3 spots, each with direction, distance and the level range of the mobs
  there, e.g. `Explore NE, 300 yd (boars, scorpids, level 3-5)`. The spots come from playerbots'
  per-level spot cache (`sTravelMgr.GetLocsPerLevelCache`, what `SelectRandomGrindPos` uses),
  filtered to the bot's zone, one per direction, nearest first (with a fallback to walkable
  `mmap` rays ~80 yd out when a zone/instance has no cached grind spots). What is there is a
  fact; whether to go is the brain's call.
- **When:** alive, out of combat.
- **Carried out:** a long walk (below). Done on arrival.

## Go to <place>

- **Targets:** the nearest place of each kind the bot could need, from creature spawns with that
  NPC flag and playerbots' travel hubs (`sTravelMgr.GetTravelHubs`), grouped by hub/subzone so a
  single village isn't listed four times: innkeeper, vendor/repair, the bot's class trainer,
  flight master, graveyard (and active quest objective/turn-in area when `quest_tracker` has a
  known `where`), e.g. `Go to Razor Hill (innkeeper, vendor, repair), 900 yd N`.
- **Snapshot visibility:** nearby places within minimap range also populate `ext.minimap.places`
  (always-on `glance` visibility, without `Place::level_range` which is `hover`-only), while
  `ext.map.places` stays gated on `Panel::MAP` in `base.ui.open_panels` (`snapshot.md` §5,
  enforced by `ValidateSnapshotVisibility`). In offline `fixture_options`, `AddOptions` reads
  places from `OptionFacts` (the same list the live module uses).
- **When:** alive, out of combat.
- **Carried out:** a long walk. Done on arrival (within 10 yd). The hearthstone stays a separate
  thing (a later `Use Hearthstone` item option).
- **Not while dead:** a ghost still gets only Keep. Playerbots' dead strategy walks the ghost to
  its corpse, and a `Go to graveyard` would fight it for the movement. Death options (release
  spirit, run to corpse, spirit healer) are their own later item.

## The long walk (Explore and Go to) & harness wiring

- **Stepped `mmap` walk:** our `WalkTo` today is a single `MovePoint`: fine for 20 yd, not for
  900. Instead, `OptionExecutor` stores a per-bot pending walk target and advances in steps of
  ~70 yd along the `mmap` path on world ticks whenever the bot gets within ~10 yd of the current
  step, refreshing playerbots' `last movement` so its own movement doesn't override ours. We
  **don't** use `MoveFarTo` itself: when stuck for 90 s it teleports the bot (a cheat). If path
  generation fails or progress stalls (the bot moved less than 5 yd in 10 s: a detour that leads
  away from the goal for a while still counts as moving), we abort the walk and send `"stuck"` as
  a refused-step `error` event (carrying the step's `command_id`), and the brain decides what to
  do. Combat, death or another map end the walk with `walk interrupted: ...`, so the harness
  doesn't wait for its backstop. The path is asked for again at most once a second. Arrival
  within 10 yd sets `done_command_id`.
  Steps are needed anyway: AzerothCore's `PathGenerator` cuts a path at
  `MAX_POINT_PATH_LENGTH` (74 points), so one `MovePoint` can't cover 900 yd.
- **World-thread cost:** one path per ~70 yd step per walking bot runs on the world thread,
  which our module must never block: `perf walk` (50 bots walking at once). The facts: the
  world's spots and hubs are indexed once at startup (`WorldPlaces::Build`, 76 ms); a bot's spots
  and places are recomputed after 20 yd or 10 s and only re-measured in between
  (`SnapshotBuilder`); quest map points are cached per quest (`perf snapshots`).
- **Distinct `ScreenOption::name` for multiple `Explore` / `Go to` lines:** in
  `harness/src/screen_renderer.cc`, `ParseModuleOption` currently sets `ScreenOption::name = ""`
  for `kExplore` and `kTown`. Because `PlanRunner::MatchOptionInScreen` matches queued steps by
  `(wanted.kind, wanted.name)`, multiple `Explore` or `Go to` options on one screen would
  collide on `""` and always dispatch the first one. The text can't be the key either: the
  distance and direction in it change as the bot walks (`NE, 300 yd` becomes `N, 280 yd`), so a
  queued step would stop matching its own option. Instead:
  - each spot and place gets a stable id in the option's `guid` (`place:<hub id>`,
    `spot:<map>:<x>:<y>`), which `MatchOptionInScreen` already tries first;
  - `ScreenOption::name` keeps only the part that doesn't change: the place name, or what lives
    at the spot (`boars, scorpids, level 3-5`), never the distance or direction;
  - the spots are chosen the same way from one snapshot to the next (nearest per direction from
    the cache), never at random: `SelectRandomGrindPos` picks with `urand`, so we use its spot
    list, not its pick. Otherwise the options flicker every snapshot.
- **Step timeout:** the module's stall check (less than 5 yd closer in 10 s, then a refused
  step) is the real guard, in the place that sees the bot move. The harness timeout for
  `kExplore` and `kTown` becomes only a generous backstop (10 min) instead of a flat 60 s; no
  parsing `<N> yd` out of the option text.
- **While touching `options.cc`:** also gate `Gather <herb/ore>` on `required_skill` vs. the
  bot's learned Herbalism/Mining skill (TODO loose end #1) so bots without the profession stop
  walking to nodes they can't gather. Hidden, not annotated: an option is something the bot can
  do.

## One code path, live and offline

Options must be computed by the same code in the live module and in `fixture_options`, or the
eval tests something live bots never see (review 2026-11-17, finding 5). So `AddOptions` reads
only the snapshot plus an `OptionFacts` struct: the game data a player knows but the snapshot
doesn't show (NPC flags by guid, food and drink in the bags, nearby places with their services,
learned gathering skills). Spot and place ids come from their position (`spot:<map>:<x>:<y>`,
`place:<map>:<x>:<y>`), never from a name two places can share. Each `Explore` spot and `Go to` place in `OptionFacts` carries its
destination `pb::Vec3 pos`, which `AddOptions` copies onto `OptionTarget::pos` so
`OptionExecutor` walks straight to that coordinate without re-querying `sTravelMgr`. The live
module builds `OptionFacts` from the game. For the eval fixtures the test server runs the same
code on each saved snapshot (`LlmBrain.TestFactsQueries`, `test/facts_dump.cc`;
`tools/snapshot/add_fixture_options.py --capture`) and writes `<case>.facts.json`, which
`fixture_options` reads. What a hand-built scene's snapshot doesn't show (team, food and drink,
gathering skills) is stated in the case's `scene` (defaults: Horde, the starting kit's bread and
water, no gathering skill); NPC flags come from the creature entry in each unit's guid. The
walkable-ray Explore fallback needs a live bot, so the dump skips it.

## Decided (2026-11-17)

1. **Talk:** do everything on arrival now (`AcceptQuest` / `TurnInQuest` with `BestRewardIndex`,
   learn affordable trainer spells, sell greys + repair, a short `info` summary); window choices
   with `ext.interaction` later.
2. **Explore:** spots from `sTravelMgr.GetLocsPerLevelCache`, labeled with what lives there, with
   the walkable `mmap` ray fallback when none are cached.
3. **Go to:** hub-grouped innkeeper, vendor/repair, class trainer, flight master, graveyard, plus
   the known quest objective / turn-in `where`. Not while dead.
4. **Eval fixtures:** the 13 `planned` cases are re-captured live (so each has its
   `.facts.json`), run through `fixture_options` and re-judged once the options land.
