# Magisters' Terrace (MgT)

<!-- DRAFT extracted from mod-playerbots (Dungeon/MgT); hand-check before use. -->

## Selin Fireheart

- **Mgt Out of Room** (positioning): Stay inside the boss room to avoid leashing/resetting.
- **Crystal** (add_switch): Priority add; switch targets to `Crystal` immediately.
- **Fel Crystal** (add_switch): Priority add (`NPC_FEL_CRYSTAL`); switch and burn immediately.

```
boss beyond 30 -> to boss
Crystal appears -> target nearest Crystal
Fel Crystal appears -> target nearest Fel Crystal
```

## Magisters' Terrace

- **Dampening Field** (ground_hazard): Ground hazard (~5 yd); move out immediately.
- **Mgt Priority Interrupt** (interrupt): Priority interrupt target: Mgt Priority Interrupt.
- **Arcane Nova** (cast_aoe): Boss cast AoE (16 yd radius); move out during cast.

```
in Dampening Field -> out of it
Arcane Nova cast -> out of it
```

## Priestess Delrissa

- **Mgt Delrissa Interrupt** (interrupt): Priority interrupt target: Mgt Delrissa Interrupt.
- **Tremor Totem** (debuff): Keep Tremor Totem down for Fear.

```
always -> use Tremor Totem
```

## Kael'thas Sunstrider

- **Flame Strike** (ground_hazard): Ground hazard (~7.5 yd); move out immediately.
- **Phoenix Burn** (ground_hazard): Ground hazard (~8 yd); move out immediately.
- **Mgt Kael Interrupt** (interrupt): Priority interrupt target: Mgt Kael Interrupt.
- **Mgt Gravity Lapse** (positioning): Encounter positioning (mgt gravity lapse).
- **Phoenix Egg** (add_switch): Priority add (`NPC_PHOENIX_EGG`); switch and burn immediately.

```
in Flame Strike -> out of it
in Phoenix Burn -> out of it
Phoenix Egg appears -> target nearest Phoenix Egg
```
