# Ulduar (Uld)

<!-- DRAFT extracted from mod-playerbots (Raid/Uld); hand-check before use. -->

## Flame Leviathan

- **Salvaged Siege Engine** (add_switch): Priority add; switch targets to `Salvaged Siege Engine` immediately.

```
Salvaged Siege Engine appears -> target nearest Salvaged Siege Engine
```

## Razorscale

- **Whirlwind** (cast_aoe): Boss cast AoE (8 yd radius); move out during cast.
- **Fusearmor** (debuff): Aura `Fusearmor` (me); gather/position per encounter script (hand-check).

```
Whirlwind cast -> out of it
```

## Stormcaller Brundir

- **Lightning Tendrils Man** (debuff): Aura `Lightning Tendrils Man` on boss; hand-check encounter reaction.
- **Overload Man** (debuff): Aura `Overload Man` on boss; hand-check encounter reaction.

## Iron Assembly

- **Rune of Power** (debuff): Aura `Rune of Power` on boss; hand-check encounter reaction.

## Kologarn

- **Right Arm** (add_switch): Priority add; switch targets to `Right Arm` immediately.
- **Rubble** (add_switch): Priority add; switch targets to `Rubble` immediately.
- **Crunch Armor** (debuff): Aura `Crunch Armor` on me; hand-check encounter reaction.

```
Right Arm appears -> target nearest Right Arm
Rubble appears -> target nearest Rubble
```

## Hodir

- **Snowpacked Icicle** (add_switch): Priority add; switch targets to `Snowpacked Icicle` immediately.
- **Biting Cold Player** (debuff): Aura `Biting Cold Player` on me; hand-check encounter reaction.

```
Snowpacked Icicle appears -> target nearest Snowpacked Icicle
```

## Freya

- **Bomb** (add_switch): Priority add; switch targets to `Bomb` immediately.
- **Snaplasher** (add_switch): Priority add; switch targets to `Snaplasher` immediately.
- **Healthy Spore** (add_switch): Priority add; switch targets to `Healthy Spore` immediately.

```
Bomb appears -> target nearest Bomb
Snaplasher appears -> target nearest Snaplasher
Healthy Spore appears -> target nearest Healthy Spore
```

## Thorim

- **Unbalancing Strike** (debuff): Aura `Unbalancing Strike` on me; hand-check encounter reaction.
- **Thorim Gauntlet Positioning Trigger** (positioning): Encounter positioning (thorim gauntlet positioning trigger).
- **Dark Rune Acolyte I** (add_switch): Priority add; switch targets to `Dark Rune Acolyte I` immediately.
- **Thorim Phase 2 Positioning Trigger** (positioning): Encounter positioning (thorim phase 2 positioning trigger).

```
Dark Rune Acolyte I appears -> target nearest Dark Rune Acolyte I
```

## Dark Rune Acolyte

- **Dark Rune Acolyte I** (add_switch): Priority add; switch targets to `Dark Rune Acolyte I` immediately.

```
Dark Rune Acolyte I appears -> target nearest Dark Rune Acolyte I
```

## Vx-001

- **P3wx Laser Barrage** (debuff): Aura `P3wx Laser Barrage` (boss); gather/position per encounter script (hand-check).
- **Vx** (add_switch): Priority add; switch targets to `Vx` immediately.

```
Vx appears -> target nearest Vx
```

## Leviathan Mk Ii

- **Shock Blast** (cast_aoe): Boss cast AoE (20 yd radius); move out during cast.

```
Shock Blast cast -> out of it
```

## Mimiron

- **Leviathan Mkii** (add_switch): Priority add; switch targets to `Leviathan Mkii` immediately.
- **Bomb Bot** (add_switch): Priority add; switch targets to `Bomb Bot` immediately.

```
Leviathan Mkii appears -> target nearest Leviathan Mkii
Bomb Bot appears -> target nearest Bomb Bot
```

## General Vezax

- **Vezax Shadow Crash** (ground_hazard): Ground hazard; move out immediately.
- **Mark of the Faceless** (debuff): Aura `Mark of the Faceless` (me); gather/position per encounter script (hand-check).

```
in Vezax Shadow Crash -> out of it
```

## Yogg-saron

- **Ominous Cloud** (add_switch): Priority add; switch targets to `Ominous Cloud` immediately.
- **Guardian of Ys** (add_switch): Priority add; switch targets to `Guardian of Ys` immediately.
- **Sanity** (debuff): Aura `Sanity` (me); gather/position per encounter script (hand-check).
- **Yogg Saron** (add_switch): Priority add; switch targets to `Yogg Saron` immediately.
- **Brain Link** (debuff): Aura `Brain Link` on me; hand-check encounter reaction.
- **Descend Into Madness** (add_switch): Priority add; switch targets to `Descend Into Madness` immediately.
- **Brain** (add_switch): Priority add; switch targets to `Brain` immediately.
- **Lunatic Gaze Ys** (facing): When `Lunatic Gaze Ys` is active on Yogg-saron, face away and stop attacking.
- **Immortal Guardian** (add_switch): Priority add; switch targets to `Immortal Guardian` immediately.

```
Ominous Cloud appears -> target nearest Ominous Cloud
Guardian of Ys appears -> target nearest Guardian of Ys
Yogg Saron appears -> target nearest Yogg Saron
Descend Into Madness appears -> target nearest Descend Into Madness
Brain appears -> target nearest Brain
Lunatic Gaze Ys on boss -> face away boss, then stop attacking
Immortal Guardian appears -> target nearest Immortal Guardian
```

## Ulduar

- **Sentinel Whirlwind** (cast_aoe): Boss cast AoE (`SPELL_SENTINEL_WHIRLWIND`); move out during cast.

```
Sentinel Whirlwind cast -> out of it
```
