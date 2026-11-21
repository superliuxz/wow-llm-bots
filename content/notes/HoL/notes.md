# Halls of Lightning (HoL)

<!-- DRAFT extracted from mod-playerbots (Dungeon/HoL); hand-check before use. -->

## General Bjarngrim

- **Stormforged Lieutenant** (add_switch): Priority add; switch targets to `Stormforged Lieutenant` immediately.
- **Whirlwind** (cast_aoe): Boss cast AoE (10 yd radius); move out during cast.

```
Stormforged Lieutenant appears -> target nearest Stormforged Lieutenant
Whirlwind cast -> out of it
```

## Ionar

- **Ionar Disperse** (positioning): Boss disperses into sparks; move to safe disperse position.
- **Ionar Tank Aggro** (positioning): Encounter positioning (ionar tank aggro).
- **Static Overload** (debuff): Aura `Static Overload` on boss; hand-check encounter reaction.
- **Ball Lightning** (cast_aoe): Boss cast AoE (7 yd radius); move out during cast.

```
Disperse cast -> away boss 15
Ball Lightning cast -> out of it
```

## Loken

- **Lightning Nova** (cast_aoe): Boss cast AoE (22 yd radius); move out during cast.
- **Loken Ranged** (positioning): Stack near Loken / tank.

```
Lightning Nova cast -> out of it, then back
always -> to boss
```
