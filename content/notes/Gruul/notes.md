# Gruul's Lair (Gruul)

<!-- Draft boss notes extracted from mod-playerbots (Raid/Gruul). -->

## Shared

- **High King Maulgar** (add_switch): Priority add; switch targets to `High King Maulgar` immediately.

Suggested standing orders:
```
High King Maulgar appears -> target nearest High King Maulgar
```

## High King Maulgar

- **High King Maulgar Three Ogres Need Melee Tanks** (positioning): Encounter positioning (high king maulgar three ogres need melee tanks).
- **Whirlwind** (cast_aoe): Boss cast AoE (8 yd radius); move out during cast.
- **Banish** (debuff): React to `Banish` aura.

Suggested standing orders:
```
Whirlwind cast -> out of it
Banish on me -> away boss 10
```

## Blindeye the Seer

- **Misdirection** (debuff): React to `Misdirection` aura.

Suggested standing orders:
```
Misdirection on me -> away boss 10
```

## Gruul the Dragonkiller

- **Gruul the Dragonkiller Should Be Tanked** (positioning): Encounter positioning (gruul the dragonkiller should be tanked).
- **Shatter** (cast_aoe): Boss cast AoE; move out during cast.

Suggested standing orders:
```
Shatter cast -> out of it
```
