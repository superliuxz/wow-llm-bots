# Character fixtures

Known starting states for test bots, as AzerothCore player dumps (`pdump` format). A test
config lists them in `LlmBrain.TestFixtures` (`Name=file.pdump`); on every server start the
module erases those characters and recreates them from the dumps before the bot manager logs
them in, so each run starts the same. Only characters on the test accounts
(`LlmBrain.TestAccount`, default `WLBTEST`; fixtures 11-20 go on `WLBTEST2`, and so on, 10 per
account) are ever erased.

| File | State |
|---|---|
| `priest-l1-valley.pdump` | Troll Priest, level 1, freshly created (never logged in), at the Valley of Trials start with starting gear and hearthstone |
| `priest-l1-boars.pdump` | The same fresh priest moved about 12 yd from a Mottled Boar spawn in the Valley of Trials (-669, -4282); used by the choose test |
| `city-orgrimmar.pdump` | The same Troll Priest after its first login, standing in Orgrimmar's Valley of Strength (bank and auction house); used by the snapshot pressure test |

## Making a new fixture

1. Get a character into the state you want (play it, or let a bot play it).
2. With the character offline: `tools/wlb console pdump write <file>.pdump <Name>` (written to
   `out/server/bin/`).
3. Read the file, move it here, add a row above. The character's original name, account and
   GUID are replaced when it's loaded.
