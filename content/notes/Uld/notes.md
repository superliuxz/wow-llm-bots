# Ulduar (Uld)

<!-- Draft boss notes extracted from mod-playerbots (Raid/Uld). -->

## Flame Leviathan

- **Flame** (ground_hazard): Ground hazard (~3.5 yd); move out immediately.

Suggested standing orders:
```
in Flame -> out of it
```

## Razorscale

- **Flame** (ground_hazard): Ground hazard (~3.5 yd); move out immediately.
- **Whirlwind** (cast_aoe): Boss cast AoE (8 yd radius); move out during cast.
- **Fusearmor** (debuff): React to `Fusearmor` aura.
- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Flame -> out of it
Whirlwind cast -> out of it
Fusearmor on me -> away boss 10
in Fire -> out of it
```

## Ignis

- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Fire -> out of it
```

## Stormcaller Brundir

- **Lightning Tendrils 10 Man** (debuff): Debuff; spread 18 yd away from allies.
- **Overload 10 Man** (debuff): Debuff; spread 20 yd away from allies.

Suggested standing orders:
```
Lightning Tendrils 10 Man on me -> away group 18
Overload 10 Man on me -> away group 20
```

## Iron Assembly

- **Rune of Power** (debuff): Debuff; spread 10 yd away from allies.

Suggested standing orders:
```
Rune of Power on me -> away group 10
```

## Kologarn

- **Right Arm** (add_switch): Priority add; switch targets to `Right Arm` immediately.
- **Rubble** (add_switch): Priority add; switch targets to `Rubble` immediately.
- **Crunch Armor** (debuff): React to `Crunch Armor` aura.

Suggested standing orders:
```
Right Arm appears -> target nearest Right Arm
Rubble appears -> target nearest Rubble
Crunch Armor on me -> away boss 10
```

## Hodir

- **Snowpacked Icicle** (add_switch): Priority add; switch targets to `Snowpacked Icicle` immediately.
- **Biting Cold Player** (debuff): React to `Biting Cold Player` aura.

Suggested standing orders:
```
Snowpacked Icicle appears -> target nearest Snowpacked Icicle
Biting Cold Player on me -> away boss 10
```

## Freya

- **Bomb** (add_switch): Priority add; switch targets to `Bomb` immediately.
- **Fire** (ground_hazard): Ground hazard; move out immediately.
- **Snaplasher** (add_switch): Priority add; switch targets to `Snaplasher` immediately.
- **Healthy Spore** (add_switch): Priority add; switch targets to `Healthy Spore` immediately.

Suggested standing orders:
```
Bomb appears -> target nearest Bomb
in Fire -> out of it
Snaplasher appears -> target nearest Snaplasher
Healthy Spore appears -> target nearest Healthy Spore
```

## Thorim

- **Unbalancing Strike** (debuff): React to `Unbalancing Strike` aura.
- **Thorim Gauntlet Positioning Trigger** (positioning): Encounter positioning (thorim gauntlet positioning trigger).
- **Dark Rune Acolyte I** (add_switch): Priority add; switch targets to `Dark Rune Acolyte I` immediately.
- **Thorim Phase 2 Positioning Trigger** (positioning): Encounter positioning (thorim phase 2 positioning trigger).

Suggested standing orders:
```
Unbalancing Strike on me -> away boss 10
Dark Rune Acolyte I appears -> target nearest Dark Rune Acolyte I
```

## Dark Rune Acolyte

- **Dark Rune Acolyte I** (add_switch): Priority add; switch targets to `Dark Rune Acolyte I` immediately.

Suggested standing orders:
```
Dark Rune Acolyte I appears -> target nearest Dark Rune Acolyte I
```

## Vx-001

- **P3wx2 Laser Barrage 1** (debuff): React to `P3wx2 Laser Barrage 1` aura.
- **Vx001** (add_switch): Priority add; switch targets to `Vx001` immediately.

Suggested standing orders:
```
P3wx2 Laser Barrage 1 on me -> away boss 10
Vx001 appears -> target nearest Vx001
```

## Leviathan Mk Ii

- **Shock Blast** (cast_aoe): Boss cast AoE (20 yd radius); move out during cast.

Suggested standing orders:
```
Shock Blast cast -> out of it
```

## Mimiron

- **Fire** (ground_hazard): Ground hazard; move out immediately.
- **Leviathan Mkii** (add_switch): Priority add; switch targets to `Leviathan Mkii` immediately.
- **Bomb Bot** (add_switch): Priority add; switch targets to `Bomb Bot` immediately.

Suggested standing orders:
```
in Fire -> out of it
Leviathan Mkii appears -> target nearest Leviathan Mkii
Bomb Bot appears -> target nearest Bomb Bot
```

## General Vezax

- **Shadow Crash** (ground_hazard): Ground hazard; move out immediately.
- **Mark of the Faceless** (debuff): React to `Mark of the Faceless` aura.

Suggested standing orders:
```
in Shadow Crash -> out of it
Mark of the Faceless on me -> away boss 10
```

## Yogg-saron

- **Ominous Cloud** (add_switch): Priority add; switch targets to `Ominous Cloud` immediately.
- **Guardian of Ys** (add_switch): Priority add; switch targets to `Guardian of Ys` immediately.
- **Sanity** (debuff): React to `Sanity` aura.
- **Yogg Saron** (add_switch): Priority add; switch targets to `Yogg Saron` immediately.
- **Brain Link** (debuff): React to `Brain Link` aura.
- **Descend Into Madness** (add_switch): Priority add; switch targets to `Descend Into Madness` immediately.
- **Brain** (add_switch): Priority add; switch targets to `Brain` immediately.
- **Lunatic Gaze Ys** (facing): When `Lunatic Gaze Ys` is active on Yogg-saron, face away and stop attacking.
- **Immortal Guardian** (add_switch): Priority add; switch targets to `Immortal Guardian` immediately.

Suggested standing orders:
```
Ominous Cloud appears -> target nearest Ominous Cloud
Guardian of Ys appears -> target nearest Guardian of Ys
Sanity on me -> away boss 10
Yogg Saron appears -> target nearest Yogg Saron
Brain Link on me -> away boss 10
Descend Into Madness appears -> target nearest Descend Into Madness
Brain appears -> target nearest Brain
Lunatic Gaze Ys on boss -> face away boss, then stop attacking
Immortal Guardian appears -> target nearest Immortal Guardian
```
