# The Ruby Sanctum (RS)

## Baltharus the Warborn

- **Enervating Brand** (debuff): Aura `Enervating Brand` (me); gather/position per encounter script (hand-check).
- **Baltharus the Warborn Clone** (add_switch): Priority add; switch targets to `Baltharus the Warborn Clone` immediately.

## Saviana Ragefire

- **Flame Beacon** (debuff): Aura `Flame Beacon` (me); gather/position per encounter script (hand-check).
- **Rs Saviana Tank Position** (positioning): Encounter positioning (rs saviana tank position).

## General Zarithrian

- **Add** (add_switch): Priority add; switch targets to `Add` immediately.

## The Ruby Sanctum

- **Rs Halion Start Position** (positioning): Encounter positioning (rs halion start position).
- **Fiery Combustion** (debuff): Aura `Fiery Combustion` (me); gather/position per encounter script (hand-check).
- **Living Inferno** (add_switch): Priority add; switch targets to `Living Inferno` immediately.
- **Add** (add_switch): Priority add; switch targets to `Add` immediately.
- **Portal** (add_switch): Priority add; switch targets to `Portal` immediately.
- **Orb Carrier** (add_switch): Priority add; switch targets to `Orb Carrier` immediately.
- **Soul Consumption** (debuff): Aura `Soul Consumption` (me); gather/position per encounter script (hand-check).
- **Rs Halion P2 Tank Position** (positioning): Encounter positioning (rs halion p2 tank position).
- **Flame Breath** (cast_aoe): Boss cast AoE (`SPELL_FLAME_BREATH`); move out during cast.
- **Flame Breath Alt** (cast_aoe): Boss cast AoE (`SPELL_FLAME_BREATH_ALT1`); move out during cast.
- **Cleave** (cast_aoe): Boss cast AoE (`SPELL_CLEAVE`); move out during cast.
- **Dark Breath** (cast_aoe): Boss cast AoE (`SPELL_DARK_BREATH`); move out during cast.

```
Living Inferno appears -> target nearest Living Inferno
Orb Carrier appears -> target nearest Orb Carrier
Flame Breath cast -> out of it
Flame Breath cast -> out of it
Cleave cast -> out of it
Dark Breath cast -> out of it
```
