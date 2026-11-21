# Blackwing Lair (BWL)

<!-- DRAFT extracted from mod-playerbots (Raid/BWL); hand-check before use. -->

## Razorgore the Untamed

- **Mindcontrol** (debuff): Aura `Mindcontrol` (boss); gather/position per encounter script (hand-check).

## Vaelastrasz the Corrupt

- **Bwl Vaelastrasz Positioning** (positioning): Encounter positioning (bwl vaelastrasz positioning).
- **Burning Adrenaline** (debuff): Aura `Burning Adrenaline` on me; hand-check encounter reaction.

## Blackwing Lair

- **Breath** (cast_aoe): Boss cast AoE; move out during cast.
- **Brood Affliction Bronze** (debuff): Aura `Brood Affliction Bronze` on me; hand-check encounter reaction.
- **Wild Magic** (debuff): Aura `Wild Magic` on me; hand-check encounter reaction.
- **Corrupted Fire Nova Totem** (add_switch): Priority add (`NPC_CORRUPTED_FIRE_NOVA_TOTEM`); switch and burn immediately.
- **Corrupted Stoneskin Totem** (add_switch): Priority add (`NPC_CORRUPTED_STONESKIN_TOTEM`); switch and burn immediately.
- **Corrupted Healing Totem** (add_switch): Priority add (`NPC_CORRUPTED_HEALING_TOTEM`); switch and burn immediately.
- **Corrupted Windfury Totem** (add_switch): Priority add (`NPC_CORRUPTED_WINDFURY_TOTEM`); switch and burn immediately.

```
Breath cast -> out of it
Corrupted Fire Nova Totem appears -> target nearest Corrupted Fire Nova Totem
Corrupted Stoneskin Totem appears -> target nearest Corrupted Stoneskin Totem
Corrupted Healing Totem appears -> target nearest Corrupted Healing Totem
Corrupted Windfury Totem appears -> target nearest Corrupted Windfury Totem
```

## Nefarian

- **Bwl Nefarian Positioning** (positioning): Encounter positioning (bwl nefarian positioning).
- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.

```
Totem appears -> target nearest Totem
```
