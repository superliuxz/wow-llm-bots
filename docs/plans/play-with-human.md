# Plan: bots that quest, LFG and raid with a human

From the play-with-a-human code review of 2026-11-24 (findings `RH-*` in `docs/TODO.md`). Up to
then, bots had been built and measured solo or in all-bot parties. These are the directions for
playing *with* the user, in order of what is felt most in game.

1. **Popups: answer them fast, by the brain's policy.** Wake the brain on ready check, LFG role
   check, LFG proposal, loot roll, quest share and rez offer (`RH-H1`). Better still, using the
   standing-order design: the brain sets policies ahead of time ("accept rez from party", "need =
   upgrade for my spec and armor type, else greed", "LFG role = healer"). C++ applies them in under
   a second and wakes the brain only for cases no policy covers.
2. **Group roles and pull discipline (`RH-H2`, `RH-M1`).** Derive each member's role from talents
   in *every* group, not only LFG, and show it on screen. Offer `Assist <tank>`. Offer `Attack` on
   mobs not yet in combat only to the leader or puller. Change the RULES dungeon rule to "the tank
   pulls; followers assist the tank's target; never attack a pack not in combat with the party".
   Marks: skull = kill first, moon/square = CC (don't break CC, no AoE near it).
3. **Pacing like a real party.** The healer says "oom/drinking" and rests between pulls. The
   leader waits for the healer's mana and for dead members. There is a short "ready?" before boss
   pulls.
4. **The human as leader (`RH-M2`, `RH-H4`).** Map a small vocabulary of party chat to options:
   follow, wait/stay, attack my target, drink, come here, pull, go sell. For chit-chat, wake only
   the named bot, or one chosen responder, and skip lines like "ok"/"lol".
5. **Questing together (`RH-H3`).** Show party members' quest objectives on screen. Add a
   `Share quest` option. Outside instances, add a rule: stay within ~30 yd of the leader unless
   told otherwise.
6. **Loot etiquette.** Need only on main-spec upgrades you can use now, greed otherwise, pass on
   items others need. Announce wins, and never need on everything.
7. **Memory (`RH-M4`).** Write `identity.md` and `relationships.md` (`docs/design.md`, "Lifelong
   bots") so bots remember the user, past runs and wipes, and each keeps one voice. Short-term
   memory of recent conversations with each player helps most.
8. **LFG and pugs (`RH-M7`).** Listen to the LookingForGroup channel. Add a "post LFM/LFG" option.
   Fill the deferred `ext.dungeon_finder` panel.
9. **Raids.** Ready check, buffs (`missing_buffs_i_can_give` already exists), consumables, master
   loot, main tank/assist assignment, raid markers. All of these build on items 1-2.
10. **Tests for "human + bots".** Add an e2e case where a scripted human account invites 4 bots,
    queues LFG, starts a ready check, rolls on loot, types "wait"/"follow", and rezzes a dead bot.
    No e2e case has a human in the group today, which is why `RH-H1`-`RH-H4` went unnoticed.

Sources: warcraft.wiki.gg (Group loot, Etiquette); Vanilla WoW wiki "Instance grouping guide";
Engadget "WoW Rookie: knowing your place in an instance"; Wowhead "marking mobs"; mod-ollama-chat
and forks (per-player short-term memory); mod-playerbots chat commands (DeepWiki).
