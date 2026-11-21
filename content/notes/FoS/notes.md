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

- **Mirrored Soul** (facing): When `Mirrored Soul` is active on Devourer of Souls, face away and stop attacking.

```
Mirrored Soul on boss -> face away boss, then stop attacking
```

## The Forge of Souls

- **Soulstorm** (cast_aoe): Boss cast AoE (`SPELL_SOULSTORM_VISUAL`); move out during cast.

```
Soulstorm cast -> out of it
```
