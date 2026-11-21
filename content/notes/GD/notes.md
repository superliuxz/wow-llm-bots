# Gundrak (GD)

<!-- Draft boss notes extracted from mod-playerbots (Dungeon/GD). -->

## Slad'ran

- **Poison Nova** (cast_aoe): Boss cast AoE (17 yd radius); move out during cast.
- **Snake Wrap** (add_switch): Priority add; switch targets to `Snake Wrap` immediately.
- **Slad'ran Stack on Tank** (positioning): Stack near Slad'ran / tank.

Suggested standing orders:
```
Poison Nova cast -> out of it
Snake Wrap appears -> target nearest Snake Wrap
always -> to boss
```

## Gal'darah

- **Whirling Slash** (debuff): Debuff; spread 7 yd away from allies.

Suggested standing orders:
```
Whirling Slash on me -> away group 7
```
