# Molten Core (MC)

<!-- DRAFT extracted from mod-playerbots (Raid/MC); hand-check before use. -->

## Baron Geddon

- **Living Bomb** (ground_hazard): Ground hazard; move out immediately.
- **Inferno** (debuff): Aura `Inferno` on boss; hand-check encounter reaction.

```
in Living Bomb -> out of it
```

## Golemagg the Incinerator

- **Golemaggs Trust** (debuff): Aura `Golemaggs Trust` on me; hand-check encounter reaction.
- **Magma Splash** (debuff): Aura `Magma Splash` (me); gather/position per encounter script (hand-check).
- **Mc Golemagg Is Healer** (positioning): Encounter positioning (mc golemagg is healer).

## Majordomo Executus

- **Damage Shield** (debuff): Aura `Damage Shield` on me; hand-check encounter reaction.

## Core Hound

- **Hound** (add_switch): Priority add; switch targets to `Hound` immediately.

```
Hound appears -> target nearest Hound
```
