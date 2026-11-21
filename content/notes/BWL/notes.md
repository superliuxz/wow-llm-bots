# Blackwing Lair (BWL)

<!-- Draft boss notes extracted from mod-playerbots (Raid/BWL). -->

## Blackwing Lair

- **Fire** (ground_hazard): Ground hazard; move out immediately.
- **Breath** (cast_aoe): Boss cast AoE; move out during cast.
- **Flame** (ground_hazard): Ground hazard; move out immediately.
- **Brood Affliction Bronze** (debuff): React to `Brood Affliction Bronze` aura.
- **Wild Magic** (debuff): React to `Wild Magic` aura.
- **Corrupted Fire Nova Totem** (add_switch): Priority add (`NPC_CORRUPTED_FIRE_NOVA_TOTEM`); switch and burn immediately.
- **Corrupted Stoneskin Totem** (add_switch): Priority add (`NPC_CORRUPTED_STONESKIN_TOTEM`); switch and burn immediately.
- **Corrupted Healing Totem** (add_switch): Priority add (`NPC_CORRUPTED_HEALING_TOTEM`); switch and burn immediately.
- **Corrupted Windfury Totem** (add_switch): Priority add (`NPC_CORRUPTED_WINDFURY_TOTEM`); switch and burn immediately.

Suggested standing orders:
```
in Fire -> out of it
Breath cast -> out of it
in Flame -> out of it
Brood Affliction Bronze on me -> away boss 10
Wild Magic on me -> away boss 10
Corrupted Fire Nova Totem appears -> target nearest Corrupted Fire Nova Totem
Corrupted Stoneskin Totem appears -> target nearest Corrupted Stoneskin Totem
Corrupted Healing Totem appears -> target nearest Corrupted Healing Totem
Corrupted Windfury Totem appears -> target nearest Corrupted Windfury Totem
```

## Razorgore the Untamed

- **Mindcontrol** (debuff): Debuff; spread 10 yd away from allies.

Suggested standing orders:
```
Mindcontrol on me -> away group 10
```

## Vaelastrasz the Corrupt

- **Bwl Vaelastrasz Positioning** (positioning): Encounter positioning (bwl vaelastrasz positioning).
- **Burning Adrenaline** (debuff): React to `Burning Adrenaline` aura.

Suggested standing orders:
```
Burning Adrenaline on me -> away boss 10
```

## Nefarian

- **Bwl Nefarian Positioning** (positioning): Encounter positioning (bwl nefarian positioning).
- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.

Suggested standing orders:
```
Totem appears -> target nearest Totem
```
