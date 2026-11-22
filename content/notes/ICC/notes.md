# Icecrown Citadel (ICC)

<!-- DRAFT extracted from mod-playerbots (Raid/ICC); hand-check before use. -->

## Lord Marrowgar

- **Coldflame**: moving line of blue fire; move out immediately.
- **Bone Spike Graveyard**: impales players on **Bone Spike** targets; switch and destroy Bone Spikes immediately.
- **Bone Storm**: boss spins and deals heavy melee AoE while moving; move away from Marrowgar during Bone Storm.

```
in Coldflame -> out of it
Bone Spike appears -> target nearest Bone Spike
Bone Storm cast -> away Lord Marrowgar 15
```

## Lady Deathwhisper

- **Dark Reckoning** (debuff): Aura `Dark Reckoning` on me; hand-check encounter reaction.
- **Shade** (add_switch): Priority add; switch targets to `Shade` immediately.

```
Shade appears -> target nearest Shade
```

## Rotting Frost Giant

- **Icc Rotting Frost Giant Tank Position** (positioning): Encounter positioning (icc rotting frost giant tank position).

## Gunship Battle

- **Below Zero** (debuff): Aura `Below Zero` on me; hand-check encounter reaction.
- **Cannona** (add_switch): Priority add; switch targets to `Cannona` immediately.

```
Cannona appears -> target nearest Cannona
```

## Deathbringer Saurfang

- **Blood Beast** (add_switch): Priority add; switch targets to `Blood Beast` immediately.

```
Blood Beast appears -> target nearest Blood Beast
```

## Stinky

- **Mortal Wound** (debuff): Aura `Mortal Wound` (me); gather/position per encounter script (hand-check).

## Festergut

- **Icc Festergut Group Position** (positioning): Encounter positioning (icc festergut group position).
- **Gas Spore** (debuff): Aura `Gas Spore` on me; hand-check encounter reaction.

## Rotface

- **Icc Rotface Tank Position** (positioning): Encounter positioning (icc rotface tank position).
- **Icc Rotface Group Position** (positioning): Encounter positioning (icc rotface group position).
- **Explosion** (cast_aoe): Boss cast AoE; move out during cast.
- **Vile Gas** (ground_hazard): Ground hazard; move out immediately.

```
Explosion cast -> out of it, then back
in Vile Gas -> out of it
```

## Volatile Ooze

- **Ooze** (add_switch): Priority add; switch targets to `Ooze` immediately.

```
Ooze appears -> target nearest Ooze
```

## Gas Cloud

- **Gas Cloud** (ground_hazard): Ground hazard; move out immediately.

```
in Gas Cloud -> out of it
```

## Professor Putricide

- **Ooze Puddle** (ground_hazard): Ground hazard (~8 yd); move out immediately.
- **Spitefull Fury** (ground_hazard): Ground hazard; move out immediately.
- **Mutated Abomination** (add_switch): Priority add; switch targets to `Mutated Abomination` immediately.

```
in Ooze Puddle -> out of it
in Spitefull Fury -> out of it
Mutated Abomination appears -> target nearest Mutated Abomination
```

## Prince Keleseth

- **Dark Nucleus** (add_switch): Priority add; switch targets to `Dark Nucleus` immediately.

```
Dark Nucleus appears -> target nearest Dark Nucleus
```

## Prince Valanar

- **Empowered Shock Vortex** (cast_aoe): Boss cast AoE; move out during cast.
- **Ball of Flame** (add_switch): Priority add; switch targets to `Ball of Flame` immediately.

```
Empowered Shock Vortex cast -> out of it
Ball of Flame appears -> target nearest Ball of Flame
```

## Blood Prince Council

- **Kinetic Bomb** (add_switch): Priority add; switch targets to `Kinetic Bomb` immediately.

```
Kinetic Bomb appears -> target nearest Kinetic Bomb
```

## Blood-queen Lana'thel

- **Frenzied Bloodthirst** (debuff): Aura `Frenzied Bloodthirst` on me; hand-check encounter reaction.
- **Pact of the Darkfallen** (debuff): Aura `Pact of the Darkfallen` (me); gather/position per encounter script (hand-check).

## Sister Svalna

- **Aether Shield** (debuff): Aura `Aether Shield` on me; hand-check encounter reaction.

## Valithria Dreamwalker

- **Dream State** (cast_aoe): Boss cast AoE (11 yd radius); move out during cast.
- **Kite Blistering Zombie** (kite): Kite `Blistering Zombie` away from the group.
- **Dream Portal** (add_switch): Priority add; switch targets to `Dream Portal` immediately.
- **Dream Cloud** (add_switch): Priority add; switch targets to `Dream Cloud` immediately.

```
Dream State cast -> out of it
Blistering Zombie targets me -> kite it
Dream Portal appears -> target nearest Dream Portal
Dream Cloud appears -> target nearest Dream Cloud
```

## Sindragosa

- **Tomb** (add_switch): Priority add; switch targets to `Tomb` immediately.
- **Frost Beacon** (debuff): Aura `Frost Beacon` on me; hand-check encounter reaction.
- **Blistering Cold** (cast_aoe): Boss cast AoE; move out during cast.
- **Unchained Magic** (debuff): Aura `Unchained Magic` (me); gather/position per encounter script (hand-check).
- **Chilled to the Bone** (debuff): Aura `Chilled to the Bone` (me); gather/position per encounter script (hand-check).
- **Ice Tomb** (facing): When `Ice Tomb` is active on Sindragosa, face away and stop attacking.

```
Tomb appears -> target nearest Tomb
Blistering Cold cast -> out of it
Ice Tomb on boss -> face away boss, then stop attacking
```

## The Lich King

- **Valithria Dreamwalker** (add_switch): Priority add; switch targets to `Valithria Dreamwalker` immediately.
- **Nitro Boosts** (ground_hazard): Ground hazard; move out immediately.

```
Valithria Dreamwalker appears -> target nearest Valithria Dreamwalker
in Nitro Boosts -> out of it
```

## Icecrown Citadel

- **Spike** (add_switch): Priority add (`NPC_SPIKE1`); switch and burn immediately.
- **Choking Gas Bomb** (add_switch): Priority add (`NPC_CHOKING_GAS_BOMB`); switch and burn immediately.
- **Spirit Bomb** (add_switch): Priority add (`NPC_SPIRIT_BOMB`); switch and burn immediately.
- **Unstable Ooze Explosion** (cast_aoe): Boss cast AoE (`SPELL_UNSTABLE_OOZE_EXPLOSION`); move out during cast.
- **Quake** (cast_aoe): Boss cast AoE (`SPELL_QUAKE`); move out during cast.

```
Spike appears -> target nearest Spike
Choking Gas Bomb appears -> target nearest Choking Gas Bomb
Spirit Bomb appears -> target nearest Spirit Bomb
Unstable Ooze Explosion cast -> out of it
Quake cast -> out of it
```
