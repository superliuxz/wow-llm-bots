# Magtheridon's Lair (Mag)

<!-- DRAFT extracted from mod-playerbots (Raid/Mag); hand-check before use. -->

## Magtheridon's Lair

- **Hellfire Channeler** (add_switch): Priority add; switch targets to `Hellfire Channeler` immediately.
- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **Banish** (debuff): Aura `Banish` on me; hand-check encounter reaction.
- **Blast Nova** (cast_aoe): Boss cast AoE (`SPELL_BLAST_NOVA`); move out during cast.
- **Quake** (cast_aoe): Boss cast AoE (`SPELL_QUAKE`); move out during cast.

```
Hellfire Channeler appears -> target nearest Hellfire Channeler
Blast Nova cast -> out of it
Quake cast -> out of it
```

## Magtheridon

- **Magtheridon Should Be Tanked** (positioning): Encounter positioning (magtheridon should be tanked).
