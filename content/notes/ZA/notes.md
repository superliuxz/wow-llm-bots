# Zul'Aman (ZA)

<!-- DRAFT extracted from mod-playerbots (Raid/ZA); hand-check before use. -->

## Shared

- **Hex Lord Malacrass** (add_switch): Priority add; switch targets to `Hex Lord Malacrass` immediately.

```
Hex Lord Malacrass appears -> target nearest Hex Lord Malacrass
```

## Amani'shi Medicine Man

- **Amani Healing Ward** (add_switch): Priority add; switch targets to `Amani Healing Ward` immediately.

```
Amani Healing Ward appears -> target nearest Amani Healing Ward
```

## Trash

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.
- **Akil'zon Should Be Tanked** (positioning): Encounter positioning (akil'zon should be tanked).
- **Jan'alai Should Be Tanked** (positioning): Encounter positioning (jan'alai should be tanked).
- **Breath** (cast_aoe): Boss cast AoE; move out during cast.
- **Halazzi Should Be Tanked** (positioning): Encounter positioning (halazzi should be tanked).

```
Breath cast -> out of it
```

## Nalorakk

- **Nalorakk Both Forms Should Be Tanked** (positioning): Encounter positioning (nalorakk both forms should be tanked).

## Spirit of the Lynx

- **Spirit** (add_switch): Priority add; switch targets to `Spirit` immediately.

```
Spirit appears -> target nearest Spirit
```

## Halazzi

- **Corrupted Lightning Totem** (add_switch): Priority add; switch targets to `Corrupted Lightning Totem` immediately.

```
Corrupted Lightning Totem appears -> target nearest Corrupted Lightning Totem
```

## Hex Lord Malacrass

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.
- **Alyson Antille** (add_switch): Priority add; switch targets to `Alyson Antille` immediately.
- **Whirlwind** (cast_aoe): Boss cast AoE; move out during cast.

```
Alyson Antille appears -> target nearest Alyson Antille
Whirlwind cast -> out of it
```

## Zul'jin

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.
- **Zul'jin Should Be Tanked** (positioning): Encounter positioning (zul'jin should be tanked).
- **Whirlwind** (cast_aoe): Boss cast AoE; move out during cast.
- **Zul'jin Summoning Cyclones in Eagle Form** (positioning): Encounter positioning (zul'jin summoning cyclones in eagle form).

```
Whirlwind cast -> out of it
```

## Zul'Aman

- **Hex Lord Whirlwind** (cast_aoe): Boss cast AoE (`SPELL_HEX_LORD_WHIRLWIND`); move out during cast.
- **Zuljin Whirlwind** (cast_aoe): Boss cast AoE (`SPELL_ZULJIN_WHIRLWIND`); move out during cast.
- **Fire Bomb** (add_switch): Priority add (`NPC_FIRE_BOMB`); switch and burn immediately.

```
Hex Lord Whirlwind cast -> out of it
Zuljin Whirlwind cast -> out of it
Fire Bomb appears -> target nearest Fire Bomb
```
