# Tempest Keep (TK)

<!-- Draft boss notes extracted from mod-playerbots (Raid/TK). -->

## Shared

- **Lord Sanguinar** (add_switch): Priority add; switch targets to `Lord Sanguinar` immediately.

Suggested standing orders:
```
Lord Sanguinar appears -> target nearest Lord Sanguinar
```

## Al'ar

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Ember of Alar** (add_switch): Priority add; switch targets to `Ember of Alar` immediately.
- **Flame** (ground_hazard): Ground hazard (~15 yd); move out immediately.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Ember of Alar appears -> target nearest Ember of Alar
in Flame -> out of it
```

## Trash

- **Flame** (ground_hazard): Ground hazard (~15 yd); move out immediately.

Suggested standing orders:
```
in Flame -> out of it
```

## Void Reaver

- **Void Reaver Should Be Tanked** (positioning): Encounter positioning (void reaver should be tanked).

## Solarium Agent

- **Solarium Priest** (add_switch): Priority add; switch targets to `Solarium Priest` immediately.

Suggested standing orders:
```
Solarium Priest appears -> target nearest Solarium Priest
```

## Thaladred the Darkener

- **Kite Thaladred the Darkener** (kite): Kite `Thaladred the Darkener` away from the group.

Suggested standing orders:
```
Thaladred the Darkener targets me -> kite it
```

## Grand Astromancer Capernian

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Kael'thas Sunstrider Capernian Should Be Tanked by Warlock** (positioning): Encounter positioning (kael'thas sunstrider capernian should be tanked by warlock).

Suggested standing orders:
```
Misdirection on me -> away boss 10
```

## Lord Sanguinar

- **Kael'thas Sunstrider Sanguinar or Telonicus Should Be Tanked** (positioning): Encounter positioning (kael'thas sunstrider sanguinar or telonicus should be tanked).

## High Astromancer Solarian

- **Kael'thas Sunstrider Should Hold Phase 3 Positions** (positioning): Encounter positioning (kael'thas sunstrider should hold phase 3 positions).
- **Netherstrand Longbow** (add_switch): Priority add; switch targets to `Netherstrand Longbow` immediately.
- **Devastation** (add_switch): Priority add; switch targets to `Devastation` immediately.
- **Kael'thas Mind Control** (debuff): React to `Kael'thas Mind Control` aura.
- **Gravity Lapse** (debuff): Debuff; spread 10 yd away from allies.

Suggested standing orders:
```
Netherstrand Longbow appears -> target nearest Netherstrand Longbow
Devastation appears -> target nearest Devastation
Kael'thas Mind Control on me -> away boss 10
Gravity Lapse on me -> away group 10
```

## Kael'thas Sunstrider

- **Flame Strike** (ground_hazard): Ground hazard (~15 yd); move out immediately.
- **Phoenix Egg** (add_switch): Priority add (`NPC_PHOENIX_EGG`); switch and burn immediately.

Suggested standing orders:
```
in Flame Strike -> out of it
Phoenix Egg appears -> target nearest Phoenix Egg
```
