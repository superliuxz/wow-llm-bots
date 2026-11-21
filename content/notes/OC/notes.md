# The Oculus (OC)

<!-- Draft boss notes extracted from mod-playerbots (Dungeon/OC). -->

## Drakos the Interrogator

- **Unstable Sphere** (add_switch): Priority add; switch targets to `Unstable Sphere` immediately.

Suggested standing orders:
```
Unstable Sphere appears -> target nearest Unstable Sphere
```

## Ley-guardian Eregos

- **Planar Shift** (debuff): React to `Planar Shift` aura.

Suggested standing orders:
```
Planar Shift on me -> away boss 10
```

## Drakes

- **Amber Drake** (add_switch): Priority add; switch targets to `Amber Drake` immediately.

Suggested standing orders:
```
Amber Drake appears -> target nearest Amber Drake
```

## Mage-lord Urom

- **Arcane Explosion** (cast_aoe): Boss cast AoE; move out during cast.
- **Time Bomb** (debuff): Debuff; spread 12 yd away from allies.

Suggested standing orders:
```
Arcane Explosion cast -> out of it, then back
Time Bomb on me -> away group 12
```
