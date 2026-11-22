# The Forge of Souls (FoS)

<!-- DRAFT extracted from mod-playerbots (Dungeon/FoS); hand-check before use. -->

## Bronjahm

- **Corrupt Soul** (cast_aoe): Boss cast AoE (15 yd radius); move out during cast.
- **Corrupted Soul Fragment** (add_switch): Priority add; switch targets to `Corrupted Soul Fragment` immediately.

```
Corrupt Soul cast -> out of it
Corrupted Soul Fragment appears -> target nearest Corrupted Soul Fragment
```

## Devourer of Souls

- **Mirrored Soul** (reflect): Reflects 45% of damage dealt to Devourer onto the linked party member; stop attacking while active on anyone.
- **Well of Souls** (ground_hazard): Purple void zone on the ground; move out immediately.

```
Mirrored Soul on anyone -> stop attacking
in Well of Souls -> out of it
```

## The Forge of Souls

- **Soulstorm** (cast_aoe): Boss cast AoE (`SPELL_SOULSTORM_VISUAL`); move out during cast.

```
Soulstorm cast -> out of it
```
