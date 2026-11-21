# Zul'Aman (ZA)

<!-- Draft boss notes extracted from mod-playerbots (Raid/ZA). -->

## Shared

- **Hex Lord Malacrass** (add_switch): Priority add; switch targets to `Hex Lord Malacrass` immediately.

Suggested standing orders:
```
Hex Lord Malacrass appears -> target nearest Hex Lord Malacrass
```

## Amani'shi Medicine Man

- **Amani Healing Ward** (add_switch): Priority add; switch targets to `Amani Healing Ward` immediately.

Suggested standing orders:
```
Amani Healing Ward appears -> target nearest Amani Healing Ward
```

## Trash

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Akil'zon Should Be Tanked** (positioning): Encounter positioning (akil'zon should be tanked).
- **Jan'alai Should Be Tanked** (positioning): Encounter positioning (jan'alai should be tanked).
- **Flame** (ground_hazard): Ground hazard; move out immediately.
- **Fire** (ground_hazard): Ground hazard (~40 yd); move out immediately.
- **Halazzi Should Be Tanked** (positioning): Encounter positioning (halazzi should be tanked).

Suggested standing orders:
```
Misdirection on me -> away boss 10
in Flame -> out of it
in Fire -> out of it
```

## Nalorakk

- **Nalorakk Both Forms Should Be Tanked** (positioning): Encounter positioning (nalorakk both forms should be tanked).

## Spirit of the Lynx

- **Spirit** (add_switch): Priority add; switch targets to `Spirit` immediately.

Suggested standing orders:
```
Spirit appears -> target nearest Spirit
```

## Halazzi

- **Corrupted Lightning Totem** (add_switch): Priority add; switch targets to `Corrupted Lightning Totem` immediately.

Suggested standing orders:
```
Corrupted Lightning Totem appears -> target nearest Corrupted Lightning Totem
```

## Hex Lord Malacrass

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Alyson Antille** (add_switch): Priority add; switch targets to `Alyson Antille` immediately.
- **Whirlwind** (cast_aoe): Boss cast AoE; move out during cast.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Alyson Antille appears -> target nearest Alyson Antille
Whirlwind cast -> out of it
```

## Zul'jin

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Zul'jin Should Be Tanked** (positioning): Encounter positioning (zul'jin should be tanked).
- **Whirlwind** (cast_aoe): Boss cast AoE; move out during cast.
- **Zul'jin Summoning Cyclones in Eagle Form** (positioning): Encounter positioning (zul'jin summoning cyclones in eagle form).

Suggested standing orders:
```
Misdirection on me -> away boss 10
Whirlwind cast -> out of it
```

## Zul'Aman

- **Fire Bomb** (add_switch): Priority add (`NPC_FIRE_BOMB`); switch and burn immediately.

Suggested standing orders:
```
Fire Bomb appears -> target nearest Fire Bomb
```
