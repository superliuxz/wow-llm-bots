# Hyjal Summit (Hyjal)

<!-- Draft boss notes extracted from mod-playerbots (Raid/Hyjal). -->

## Rage Winterchill

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Rage Winterchill Should Be Tanked** (positioning): Encounter positioning (rage winterchill should be tanked).
- **Death and Decay** (ground_hazard): Ground hazard (~22 yd); move out immediately.

Suggested standing orders:
```
Misdirection on me -> away boss 10
in Death and Decay -> out of it
```

## Anetheron

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Anetheron Should Be Tanked** (positioning): Encounter positioning (anetheron should be tanked).
- **Anetheron Near Inferno Target** (ground_hazard): Ground hazard; move out immediately.
- **Anetheron Infernals Should Be Tanked Away** (positioning): Encounter positioning (anetheron infernals should be tanked away).

Suggested standing orders:
```
Misdirection on me -> away boss 10
in Anetheron Near Inferno Target -> out of it
```

## Kaz'rogal

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Kaz'rogal Should Be Tanked** (positioning): Encounter positioning (kaz'rogal should be tanked).
- **Stomp** (cast_aoe): Boss cast AoE; move out during cast.
- **Kaz'rogal Low on Mana** (ground_hazard): Ground hazard (~15 yd); move out immediately.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Stomp cast -> out of it
in Kaz'rogal Low on Mana -> out of it
```

## Azgalor

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Azgalor Should Be Tanked** (positioning): Encounter positioning (azgalor should be tanked).
- **Azgalor Ranged Should Spread** (positioning): Boss disperses into sparks; move to safe disperse position.
- **Fire** (ground_hazard): Ground hazard (~17 yd); move out immediately.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Disperse cast -> away boss 15
in Fire -> out of it
```

## Lesser Doomguard

- **Azgalor Should Control Doomguards** (positioning): Encounter positioning (azgalor should control doomguards).

## Archimonde

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Archimonde Should Be Tanked** (positioning): Encounter positioning (archimonde should be tanked).
- **Tremor Totem** (debuff): Keep Tremor Totem down for Fear.
- **Air Burst** (cast_aoe): Boss cast AoE; move out during cast.
- **Fire** (ground_hazard): Ground hazard (~17 yd); move out immediately.

Suggested standing orders:
```
Misdirection on me -> away boss 10
always -> use Tremor Totem
Air Burst cast -> out of it
in Fire -> out of it
```
