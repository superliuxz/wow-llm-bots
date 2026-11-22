# The Obsidian Sanctum (OS)

## Sartharion

- **Shadron** (add_switch): Priority add; switch targets to `Shadron` immediately.
- **Twilight Fissure** (add_switch): Priority add; switch targets to `Twilight Fissure` immediately.
- **Flame Tsunami** (add_switch): Priority add; switch targets to `Flame Tsunami` immediately.
- **Sartharion Melee Positioning** (positioning): Encounter positioning (sartharion melee positioning).
- **Portal** (add_switch): Priority add; switch targets to `Portal` immediately.

```
Shadron appears -> target nearest Shadron
Twilight Fissure appears -> target nearest Twilight Fissure
Flame Tsunami appears -> target nearest Flame Tsunami
```

## Acolyte of Shadron

- **Portal** (add_switch): Priority add; switch targets to `Portal` immediately.

## The Obsidian Sanctum

- **Shadow Breath** (cast_aoe): Boss cast AoE (`SPELL_SHADOW_BREATH`); move out during cast.
- **Sartharion Cleave** (cast_aoe): Boss cast AoE (`SPELL_SARTHARION_CLEAVE`); move out during cast.
- **Sartharion Flame Breath** (cast_aoe): Boss cast AoE (`SPELL_SARTHARION_FLAME_BREATH`); move out during cast.
- **Twilight Egg** (add_switch): Priority add (`NPC_TWILIGHT_EGG`); switch and burn immediately.
- **Safe Area** (ground_hazard): Ground hazard trigger (`NPC_SAFE_AREA_TRIGGER`); move out immediately.

```
Shadow Breath cast -> out of it
Cleave cast -> out of it
Flame Breath cast -> out of it
Twilight Egg appears -> target nearest Twilight Egg
in Safe Area -> out of it
```
