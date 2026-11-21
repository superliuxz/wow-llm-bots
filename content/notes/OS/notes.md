# The Obsidian Sanctum (OS)

<!-- Draft boss notes extracted from mod-playerbots (Raid/OS). -->

## Sartharion

- **Shadron** (add_switch): Priority add; switch targets to `Shadron` immediately.
- **Fissure** (ground_hazard): Ground hazard (~5 yd); move out immediately.
- **Flame** (ground_hazard): Ground hazard; move out immediately.
- **Sartharion Melee Positioning** (positioning): Encounter positioning (sartharion melee positioning).
- **Portal** (add_switch): Priority add; switch targets to `Portal` immediately.

Suggested standing orders:
```
Shadron appears -> target nearest Shadron
in Fissure -> out of it
in Flame -> out of it
Portal appears -> target nearest Portal
```

## Acolyte of Shadron

- **Portal** (add_switch): Priority add; switch targets to `Portal` immediately.

Suggested standing orders:
```
Portal appears -> target nearest Portal
```

## The Obsidian Sanctum

- **Twilight Egg** (add_switch): Priority add (`NPC_TWILIGHT_EGG`); switch and burn immediately.
- **Safe Area** (ground_hazard): Ground hazard trigger (`NPC_SAFE_AREA_TRIGGER`); move out immediately.

Suggested standing orders:
```
Twilight Egg appears -> target nearest Twilight Egg
in Safe Area -> out of it
```
