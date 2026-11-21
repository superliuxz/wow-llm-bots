# The Oculus (OC)

<!-- DRAFT extracted from mod-playerbots (Dungeon/OC); hand-check before use. -->

## Drakos the Interrogator

- **Unstable Sphere** (add_switch): Priority add; switch targets to `Unstable Sphere` immediately.

```
Unstable Sphere appears -> target nearest Unstable Sphere
```

## Ley-guardian Eregos

- **Planar Shift** (debuff): Aura `Planar Shift` on boss; hand-check encounter reaction.

## Drakes

- **Amber Drake** (add_switch): Priority add; switch targets to `Amber Drake` immediately.

```
Amber Drake appears -> target nearest Amber Drake
```

## Mage-lord Urom

- **Arcane Explosion** (cast_aoe): Boss cast AoE; move out during cast.
- **Time Bomb** (debuff): Debuff; spread 12 yd away from allies.

```
Arcane Explosion cast -> out of it, then back
Time Bomb on me -> away group 12
```

## The Oculus

- **Empowered Arcane Explosion** (cast_aoe): Boss cast AoE (`SPELL_EMPOWERED_ARCANE_EXPLOSION_N`); move out during cast.

```
Empowered Arcane Explosion cast -> out of it
```
