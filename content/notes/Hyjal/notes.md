# Hyjal Summit (Hyjal)

<!-- DRAFT extracted from mod-playerbots (Raid/Hyjal); hand-check before use. -->

## Rage Winterchill

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.
- **Rage Winterchill Should Be Tanked** (positioning): Encounter positioning (rage winterchill should be tanked).
- **Death and Decay** (ground_hazard): Ground hazard (~22 yd); move out immediately.

```
in Death and Decay -> out of it
```

## Anetheron

- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **Anetheron Should Be Tanked** (positioning): Encounter positioning (anetheron should be tanked).
- **Anetheron Infernals Should Be Tanked Away** (positioning): Encounter positioning (anetheron infernals should be tanked away).

## Kaz'rogal

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.
- **Kaz'rogal Should Be Tanked** (positioning): Encounter positioning (kaz'rogal should be tanked).
- **Stomp** (cast_aoe): Boss cast AoE; move out during cast.

```
Stomp cast -> out of it
```

## Azgalor

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.
- **Azgalor Should Be Tanked** (positioning): Encounter positioning (azgalor should be tanked).
- **Azgalor Ranged Should Spread** (positioning): Boss disperses into sparks; move to safe disperse position.
- **Rain of Fire** (ground_hazard): Ground hazard (~17 yd); move out immediately.

```
Disperse cast -> away boss 15
in Rain of Fire -> out of it
```

## Lesser Doomguard

- **Azgalor Should Control Doomguards** (positioning): Encounter positioning (azgalor should control doomguards).

## Archimonde

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.
- **Archimonde Should Be Tanked** (positioning): Encounter positioning (archimonde should be tanked).
- **Tremor Totem** (debuff): Keep Tremor Totem down for Fear.
- **Air Burst** (cast_aoe): Boss cast AoE; move out during cast.

```
always -> use Tremor Totem
Air Burst cast -> out of it
```
