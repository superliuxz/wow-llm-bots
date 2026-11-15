# Phase 1 results

## 1a. Stock bot, solo leveling, cheats off (2026-11-14)

Stock mod-playerbots AI (New RPG strategy), no LLM. One random bot, level 1, no human nearby.
Data: `out/tests/solo-leveling-*` (samples every 60 s via `tools/bot-sampler.sh`) and
`server/logs/Playerbots.log`.

### Final clean run (Dezlu, Troll Priest, Valley of Trials, 59 min)

| Metric | Result |
|---|---|
| Levels | 1 -> 5 (level 2 at 3 min, 3 at 12, 4 at 22, 5 at 47) |
| Quests | 8 accepted, 6 turned in |
| Deaths | 0 |
| Manager rescues (refresh / teleport) | 0 |
| Time standing still | 2 min |
| Distance travelled | ~3,400 yd, still in Durotar |
| Spells trained | **0** (still only level-1 spells at level 5) |
| Gold | **71 copper** (loot never sold; 87 items in bags) |
| Chat lines | **0** |

### What earlier runs revealed (each fixed before the clean run)

1. **Active-alone throttling:** `BotActiveAlone = 10` + smart scaling let a bot with no human
   nearby sleep ~90% of the time: it looked stuck and slow. Fixed: always active.
2. **Random-bot manager cheats:** on first idle it does `Refresh` (free revive + full heal) and a
   random level-appropriate teleport; it also hands out starting gold (24-43 g) and picks random
   spawn spots (a level-1 bot placed outside its starting valley died repeatedly).
   Fixed: `tools/disable-rndbot-cheats.sh` pre-seeds its teleport/randomize events.
3. **Stranding:** when a bot ended up somewhere with no grind spot / inn in range ("0 available"),
   it idled forever; the manager's teleport normally hides this.
4. Test tooling: PC idle-sleep paused a run (now `systemd-inhibit --what=idle`); server scripts
   tracked wrapper PIDs (fixed).

### Conclusions

- **Works out of the box:** combat, looting, the quest loop in a starting area (accept, objectives
  via quest POIs, turn in), corpse runs, human-ish early leveling pace.
- **Missing without cheats** (upkeep the stock AI leaves to shortcuts): training new spells,
  selling loot, and recovering when no destination is in range. These become goals for the brain
  (design: "Character upkeep without cheats", "Problem solving").
- **Bot tells already visible:** total chat silence, never visiting town services.
- **Management:** RandomPlayerbotMgr is built around cheats (refresh, teleports, randomize, gold,
  placement). Our module should manage our bots itself.
- **Still open for 1a:** behavior after outgrowing the starting area (finding the next hub without
  teleports) needs a longer run.
