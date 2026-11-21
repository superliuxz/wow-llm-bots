# Serpentshrine Cavern (SSC)

<!-- Draft boss notes extracted from mod-playerbots (Raid/SSC). -->

## Shared

- **Fathom Lord Karathress** (add_switch): Priority add; switch targets to `Fathom Lord Karathress` immediately.

Suggested standing orders:
```
Fathom Lord Karathress appears -> target nearest Fathom Lord Karathress
```

## Trash

- **Toxic** (ground_hazard): Ground hazard (~27 yd); move out immediately.
- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.
- **Hydross the Unstable Should Be Tanked by Frost Tank** (positioning): Encounter positioning (hydross the unstable should be tanked by frost tank).
- **Hydross the Unstable Should Be Tanked by Nature Tank** (positioning): Encounter positioning (hydross the unstable should be tanked by nature tank).

Suggested standing orders:
```
in Toxic -> out of it
Totem appears -> target nearest Totem
```

## The Lurker Below

- **The Lurker Below Should Be Tanked** (positioning): Encounter positioning (the lurker below should be tanked).
- **Guardian** (add_switch): Priority add; switch targets to `Guardian` immediately.

Suggested standing orders:
```
Guardian appears -> target nearest Guardian
```

## Leotheras the Blind

- **Vigilance** (debuff): React to `Vigilance` aura.
- **Whirlwind** (cast_aoe): Boss cast AoE (10 yd radius); move out during cast.
- **Leotheras the Blind Too Many Chaos Blast Stacks** (positioning): Stack near Leotheras the Blind / tank.

Suggested standing orders:
```
Vigilance on me -> away boss 10
Whirlwind cast -> out of it
always -> to boss
```

## Fathom-lord Karathress

- **Fathom-lord Karathress Targets Should Be Tanked** (positioning): Encounter positioning (fathom-lord karathress targets should be tanked).
- **Fathom-lord Karathress Should Heal Caribdis Tank** (positioning): Encounter positioning (fathom-lord karathress should heal caribdis tank).

## Morogrim Tidewalker

- **Morogrim Tidewalker Should Be Tanked** (positioning): Encounter positioning (morogrim tidewalker should be tanked).
- **Morogrim Tidewalker Ranged Should Stack** (positioning): Stack near Morogrim Tidewalker / tank.
- **Lady Vashj Should Be Tanked** (positioning): Encounter positioning (lady vashj should be tanked).
- **Lady Vashj Should Hold Station in Phase 2** (positioning): Encounter positioning (lady vashj should hold station in phase 2).
- **Lady Vashj Ranged Should Position in Phase 3** (positioning): Encounter positioning (lady vashj ranged should position in phase 3).
- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.
- **Lady Vashj Coilfang Strider Should Be Tanked** (positioning): Encounter positioning (lady vashj coilfang strider should be tanked).
- **Coilfang Elite** (add_switch): Priority add; switch targets to `Coilfang Elite` immediately.
- **Elemental** (add_switch): Priority add; switch targets to `Elemental` immediately.
- **Toxic** (ground_hazard): Ground hazard (~27 yd); move out immediately.

Suggested standing orders:
```
always -> to boss
Totem appears -> target nearest Totem
Coilfang Elite appears -> target nearest Coilfang Elite
Elemental appears -> target nearest Elemental
in Toxic -> out of it
```

## Serpentshrine Cavern

- **Water Elemental Totem** (add_switch): Priority add (`NPC_WATER_ELEMENTAL_TOTEM`); switch and burn immediately.
- **Spitfire Totem** (add_switch): Priority add (`NPC_SPITFIRE_TOTEM`); switch and burn immediately.

Suggested standing orders:
```
Water Elemental Totem appears -> target nearest Water Elemental Totem
Spitfire Totem appears -> target nearest Spitfire Totem
```
