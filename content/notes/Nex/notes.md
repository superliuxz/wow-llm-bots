# The Nexus (Nex)

<!-- Draft boss notes extracted from mod-playerbots (Dungeon/Nex). -->

## Horde Commander

- **Whirlwind** (cast_aoe): Boss cast AoE; move out during cast.

Suggested standing orders:
```
Whirlwind cast -> out of it
```

## Grand Magus Telestra

- **Fire** (ground_hazard): Ground hazard (~5 yd); move out immediately.
- **Telestra** (add_switch): Priority add; switch targets to `Telestra` immediately.

Suggested standing orders:
```
in Fire -> out of it
Telestra appears -> target nearest Telestra
```

## Ormorok the Tree-shaper

- **Spike** (add_switch): Priority add; switch targets to `Spike` immediately.

Suggested standing orders:
```
Spike appears -> target nearest Spike
```

## Keristrasza

- **Intense Cold** (debuff): React to `Intense Cold` aura.
- **Keristrasza Positioning** (positioning): Encounter positioning (keristrasza positioning).

Suggested standing orders:
```
Intense Cold on me -> away boss 10
```

## The Nexus

- **Crystal Spike** (add_switch): Priority add (`NPC_CRYSTAL_SPIKE`); switch and burn immediately.

Suggested standing orders:
```
Crystal Spike appears -> target nearest Crystal Spike
```
