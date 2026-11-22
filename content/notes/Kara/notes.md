# Karazhan (Kara)

## Nightbane

- **Tremor Totem** (debuff): Keep Tremor Totem down for Fear.
- **Nightbane Boss Engaged by Tanks** (positioning): Encounter positioning (nightbane boss engaged by tanks).
- **Charred Earth** (debuff): Aura `Charred Earth` (me); gather/position per encounter script (hand-check).

## Moroes

- **Add** (add_switch): Priority add; switch targets to `Add` immediately.

## Maiden of Virtue

- **Repentance** (debuff): Aura `Repentance` (me); gather/position per encounter script (hand-check).
- **Maiden of Virtue Holy Wrath Deals Chain Damage** (positioning): Encounter positioning (maiden of virtue holy wrath deals chain damage).
- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.

## The Big Bad Wolf

- **Little Red Riding Hood** (debuff): Aura `Little Red Riding Hood` (boss); gather/position per encounter script (hand-check).

## The Curator

- **The Curator Boss Engaged by Tanks** (positioning): Encounter positioning (the curator boss engaged by tanks).

## Shade of Aran

- **Arcane Explosion** (cast_aoe): Boss cast AoE; move out during cast.
- **Blizzard** (debuff): Aura `Blizzard` (me); gather/position per encounter script (hand-check).

```
Arcane Explosion cast -> out of it, then back
```

## Conjured Elemental

- **Elemental** (add_switch): Priority add; switch targets to `Elemental` immediately.

```
Conjured Elemental appears -> target nearest Conjured Elemental
```

## Netherspite

- **Red Portal** (add_switch): Priority add; switch targets to `Red Portal` immediately.
- **Blue Portal** (add_switch): Priority add; switch targets to `Blue Portal` immediately.
- **Green Portal** (add_switch): Priority add; switch targets to `Green Portal` immediately.
- **Void Zone** (ground_hazard): Ground hazard (~4 yd); move out immediately.

```
in Void Zone -> out of it
```

## Prince Malchezaar

- **Enfeeble** (debuff): Aura `Enfeeble` (me); gather/position per encounter script (hand-check).
- **Netherspite Infernal** (add_switch): Priority add; switch targets to `Netherspite Infernal` immediately.

```
Netherspite Infernal appears -> target nearest Netherspite Infernal
```
