# Karazhan (Kara)

<!-- Draft boss notes extracted from mod-playerbots (Raid/Kara). -->

## Nightbane

- **Tremor Totem** (debuff): Keep Tremor Totem down for Fear.
- **Nightbane Boss Engaged by Tanks** (positioning): Encounter positioning (nightbane boss engaged by tanks).
- **Charred Earth** (debuff): React to `Charred Earth` aura.

Suggested standing orders:
```
always -> use Tremor Totem
Charred Earth on me -> away boss 10
```

## Moroes

- **Add** (add_switch): Priority add; switch targets to `Add` immediately.

Suggested standing orders:
```
Add appears -> target nearest Add
```

## Maiden of Virtue

- **Repentance** (debuff): React to `Repentance` aura.
- **Maiden of Virtue Holy Wrath Deals Chain Damage** (positioning): Encounter positioning (maiden of virtue holy wrath deals chain damage).
- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
Repentance on me -> away boss 10
in Fire -> out of it
```

## The Big Bad Wolf

- **Little Red Riding Hood** (debuff): React to `Little Red Riding Hood` aura.

Suggested standing orders:
```
Little Red Riding Hood on me -> away boss 10
```

## Strawman

- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Fire -> out of it
```

## The Curator

- **The Curator Boss Engaged by Tanks** (positioning): Encounter positioning (the curator boss engaged by tanks).

## Shade of Aran

- **Arcane Explosion** (cast_aoe): Boss cast AoE; move out during cast.
- **Flame** (ground_hazard): Ground hazard; move out immediately.
- **Blizzard** (debuff): React to `Blizzard` aura.

Suggested standing orders:
```
Arcane Explosion cast -> out of it, then back
in Flame -> out of it
Blizzard on me -> away boss 10
```

## Conjured Elemental

- **Elemental** (add_switch): Priority add; switch targets to `Elemental` immediately.

Suggested standing orders:
```
Elemental appears -> target nearest Elemental
```

## Netherspite

- **Red Portal** (add_switch): Priority add; switch targets to `Red Portal` immediately.
- **Blue Portal** (add_switch): Priority add; switch targets to `Blue Portal` immediately.
- **Green Portal** (add_switch): Priority add; switch targets to `Green Portal` immediately.
- **Void Zone** (ground_hazard): Ground hazard (~4 yd); move out immediately.

Suggested standing orders:
```
Red Portal appears -> target nearest Red Portal
Blue Portal appears -> target nearest Blue Portal
Green Portal appears -> target nearest Green Portal
in Void Zone -> out of it
```

## Prince Malchezaar

- **Enfeeble** (debuff): React to `Enfeeble` aura.
- **Netherspite Infernal** (add_switch): Priority add; switch targets to `Netherspite Infernal` immediately.

Suggested standing orders:
```
Enfeeble on me -> away boss 10
Netherspite Infernal appears -> target nearest Netherspite Infernal
```
