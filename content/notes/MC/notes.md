# Molten Core (MC)

<!-- Draft boss notes extracted from mod-playerbots (Raid/MC). -->

## Magmadar

- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Fire -> out of it
```

## Garr

- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Fire -> out of it
```

## Baron Geddon

- **Fire** (ground_hazard): Ground hazard; move out immediately.
- **Living Bomb** (ground_hazard): Ground hazard; move out immediately.
- **Inferno** (debuff): Debuff; spread 10 yd away from allies.

Suggested standing orders:
```
in Fire -> out of it
in Living Bomb -> out of it
Inferno on me -> away group 10
```

## Sulfuron Harbinger

- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Fire -> out of it
```

## Golemagg the Incinerator

- **Fire** (ground_hazard): Ground hazard; move out immediately.
- **Golemaggs Trust** (debuff): React to `Golemaggs Trust` aura.
- **Magma Splash** (debuff): Debuff; spread 10 yd away from allies.
- **Mc Golemagg Is Healer** (positioning): Encounter positioning (mc golemagg is healer).

Suggested standing orders:
```
in Fire -> out of it
Golemaggs Trust on me -> away boss 10
Magma Splash on me -> away group 10
```

## Majordomo Executus

- **Damage Shield** (debuff): React to `Damage Shield` aura.

Suggested standing orders:
```
Damage Shield on me -> away boss 10
```

## Ragnaros

- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Fire -> out of it
```

## Core Hound

- **Hound** (add_switch): Priority add; switch targets to `Hound` immediately.

Suggested standing orders:
```
Hound appears -> target nearest Hound
```

## Trash

- **Lava** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Lava -> out of it
```
