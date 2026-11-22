# Sethekk Halls (Seth)

<!-- DRAFT extracted from mod-playerbots (Dungeon/Seth); hand-check before use. -->

## Time-lost Controller

- **Charming Totem** (add_switch): Priority add; switch targets to `Charming Totem` immediately.

```
Charming Totem appears -> target nearest Charming Totem
```

## Sethekk Prophet

- **Tremor Totem** (debuff): Keep Tremor Totem down for Fear.

```
always -> use Tremor Totem
```

## Darkweaver Syth

- **Elemental** (add_switch): Priority add; switch targets to `Elemental` immediately.

```
Elemental appears -> target nearest Elemental
```

## Anzu

- **Banish** (debuff): Aura `Banish` on me; hand-check encounter reaction.
- **Hawk Spirit** (add_switch): Priority add; switch targets to `Hawk Spirit` immediately.

```
Hawk Spirit appears -> target nearest Hawk Spirit
```

## Talon King Ikiss

- **Arcane Explosion** (cast_aoe): Boss blinks and casts a 5s AoE; move out during cast (do not return to pre-Blink spot).

```
Arcane Explosion cast -> out of it
```
