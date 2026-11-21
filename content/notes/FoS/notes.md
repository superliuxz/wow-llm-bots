# The Forge of Souls (FoS)

<!-- Draft boss notes extracted from mod-playerbots (Dungeon/FoS). -->

## Bronjahm

- **Corrupt Soul** (cast_aoe): Boss cast AoE (15 yd radius); move out during cast.
- **Corrupted Soul Fragment** (add_switch): Priority add; switch targets to `Corrupted Soul Fragment` immediately.

Suggested standing orders:
```
Corrupt Soul cast -> out of it
Corrupted Soul Fragment appears -> target nearest Corrupted Soul Fragment
```

## Devourer of Souls

- **Mirrored Soul** (facing): When `Mirrored Soul` is active on Devourer of Souls, face away and stop attacking.

Suggested standing orders:
```
Mirrored Soul on boss -> face away boss, then stop attacking
```
