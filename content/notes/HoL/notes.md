# Halls of Lightning (HoL)

<!-- Draft boss notes extracted from mod-playerbots (Dungeon/HoL). -->

## General Bjarngrim

- **Stormforged Lieutenant** (add_switch): Priority add; switch targets to `Stormforged Lieutenant` immediately.
- **Whirlwind** (cast_aoe): Boss cast AoE (10 yd radius); move out during cast.

Suggested standing orders:
```
Stormforged Lieutenant appears -> target nearest Stormforged Lieutenant
Whirlwind cast -> out of it
```

## Ionar

- **Ionar Disperse** (positioning): Boss disperses into sparks; move to safe disperse position.
- **Ionar Tank Aggro** (positioning): Encounter positioning (ionar tank aggro).
- **Static Overload** (debuff): Debuff; spread 10 yd away from allies.
- **Ball Lightning** (cast_aoe): Boss cast AoE (7 yd radius); move out during cast.

Suggested standing orders:
```
Disperse cast -> away boss 15
Static Overload on me -> away group 10
Ball Lightning cast -> out of it
```

## Loken

- **Lightning Nova** (cast_aoe): Boss cast AoE (22 yd radius); move out during cast.
- **Loken Ranged** (positioning): Stack near Loken / tank.

Suggested standing orders:
```
Lightning Nova cast -> out of it, then back
always -> to boss
```
