# The Nexus (Nex)

<!-- DRAFT extracted from mod-playerbots (Dungeon/Nex); hand-check before use. -->

## Horde Commander

- **Whirlwind** (cast_aoe): Boss cast AoE; move out during cast.

```
Whirlwind cast -> out of it
```

## Grand Magus Telestra

- **Telestra** (add_switch): Priority add; switch targets to `Telestra` immediately.

```
Telestra appears -> target nearest Telestra
```

## Ormorok the Tree-shaper

- **Spike** (add_switch): Priority add; switch targets to `Spike` immediately.

```
Spike appears -> target nearest Spike
```

## Keristrasza

- **Intense Cold** (debuff): Aura `Intense Cold` (me); gather/position per encounter script (hand-check).
- **Keristrasza Positioning** (positioning): Encounter positioning (keristrasza positioning).

## The Nexus

- **Crystal Spike** (add_switch): Priority add (`NPC_CRYSTAL_SPIKE`); switch and burn immediately.

```
Crystal Spike appears -> target nearest Crystal Spike
```
