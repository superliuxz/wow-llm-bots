# Serpentshrine Cavern (SSC)

<!-- DRAFT extracted from mod-playerbots (Raid/SSC); hand-check before use. -->

## Shared

- **Fathom Lord Karathress** (add_switch): Priority add; switch targets to `Fathom Lord Karathress` immediately.

```
Fathom Lord Karathress appears -> target nearest Fathom Lord Karathress
```

## Trash

- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.
- **Hydross the Unstable Should Be Tanked by Frost Tank** (positioning): Encounter positioning (hydross the unstable should be tanked by frost tank).
- **Hydross the Unstable Should Be Tanked by Nature Tank** (positioning): Encounter positioning (hydross the unstable should be tanked by nature tank).

```
Totem appears -> target nearest Totem
```

## The Lurker Below

- **The Lurker Below Should Be Tanked** (positioning): Encounter positioning (the lurker below should be tanked).
- **Guardian** (add_switch): Priority add; switch targets to `Guardian` immediately.

```
Guardian appears -> target nearest Guardian
```

## Leotheras the Blind

- **Vigilance** (debuff): Aura `Vigilance` on me; hand-check encounter reaction.
- **Whirlwind** (cast_aoe): Boss cast AoE (10 yd radius); move out during cast.
- **Leotheras the Blind Too Many Chaos Blast Stacks** (positioning): Stack near Leotheras the Blind / tank.

```
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

```
always -> to boss
Totem appears -> target nearest Totem
Coilfang Elite appears -> target nearest Coilfang Elite
Elemental appears -> target nearest Elemental
```

## Serpentshrine Cavern

- **Leotheras Whirlwind** (cast_aoe): Boss cast AoE (`SPELL_LEOTHERAS_WHIRLWIND`); move out during cast.
- **Water Elemental Totem** (add_switch): Priority add (`NPC_WATER_ELEMENTAL_TOTEM`); switch and burn immediately.
- **Spitfire Totem** (add_switch): Priority add (`NPC_SPITFIRE_TOTEM`); switch and burn immediately.

```
Leotheras Whirlwind cast -> out of it
Water Elemental Totem appears -> target nearest Water Elemental Totem
Spitfire Totem appears -> target nearest Spitfire Totem
```
