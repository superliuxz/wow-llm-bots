# Sethekk Halls (Seth)

<!-- Draft boss notes extracted from mod-playerbots (Dungeon/Seth). -->

## Time-lost Controller

- **Charming Totem** (add_switch): Priority add; switch targets to `Charming Totem` immediately.

Suggested standing orders:
```
Charming Totem appears -> target nearest Charming Totem
```

## Sethekk Prophet

- **Tremor Totem** (debuff): Keep Tremor Totem down for Fear.

Suggested standing orders:
```
always -> use Tremor Totem
```

## Darkweaver Syth

- **Elemental** (add_switch): Priority add; switch targets to `Elemental` immediately.

Suggested standing orders:
```
Elemental appears -> target nearest Elemental
```

## Anzu

- **Banish** (debuff): React to `Banish` aura.
- **Hawk Spirit** (add_switch): Priority add; switch targets to `Hawk Spirit` immediately.

Suggested standing orders:
```
Banish on me -> away boss 10
Hawk Spirit appears -> target nearest Hawk Spirit
```

## Talon King Ikiss

- **Talon King Ikiss Should Be Tanked** (positioning): Encounter positioning (talon king ikiss should be tanked).
- **Arcane Bubble** (debuff): React to `Arcane Bubble` aura.
- **Arcane Explosion** (cast_aoe): Boss cast AoE; move out during cast.

Suggested standing orders:
```
Arcane Bubble on me -> away boss 10
Arcane Explosion cast -> out of it, then back
```
