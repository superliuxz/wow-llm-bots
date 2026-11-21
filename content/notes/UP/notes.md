# Utgarde Pinnacle (UP)

<!-- DRAFT extracted from mod-playerbots (Dungeon/UP); hand-check before use. -->

## Grauf

- **Breath** (ground_hazard): Ground hazard (~6 yd); move out immediately.

```
in Breath -> out of it
```

## Skadi the Ruthless

- **Whirlwind** (cast_aoe): Boss cast AoE (7 yd radius); move out during cast.

```
Whirlwind cast -> out of it
```

## King Ymiron

- **Bane** (debuff): Aura `Bane` on boss; hand-check encounter reaction.

## Utgarde Pinnacle

- **Freezing Cloud Breath** (cast_aoe): Boss cast AoE (`SPELL_FREEZING_CLOUD_BREATH`); move out during cast.
- **Skadi Whirlwind** (cast_aoe): Boss cast AoE (`SPELL_SKADI_WHIRLWIND_N`); move out during cast.

```
Freezing Cloud Breath cast -> out of it
Skadi Whirlwind cast -> out of it
```
