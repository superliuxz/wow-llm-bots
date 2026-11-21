# Icecrown Citadel (ICC)

<!-- Draft boss notes extracted from mod-playerbots (Raid/ICC). -->

## Lord Marrowgar

- **Coldflame** (add_switch): Priority add; switch targets to `Coldflame` immediately.

Suggested standing orders:
```
Coldflame appears -> target nearest Coldflame
```

## Lady Deathwhisper

- **Dark Reckoning** (debuff): React to `Dark Reckoning` aura.
- **Shade** (add_switch): Priority add; switch targets to `Shade` immediately.

Suggested standing orders:
```
Dark Reckoning on me -> away boss 10
Shade appears -> target nearest Shade
```

## Rotting Frost Giant

- **Icc Rotting Frost Giant Tank Position** (positioning): Encounter positioning (icc rotting frost giant tank position).

## Gunship Battle

- **Below Zero** (debuff): React to `Below Zero` aura.
- **Fire** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
Below Zero on me -> away boss 10
in Fire -> out of it
```

## Deathbringer Saurfang

- **Blood Beast1** (add_switch): Priority add; switch targets to `Blood Beast1` immediately.

Suggested standing orders:
```
Blood Beast1 appears -> target nearest Blood Beast1
```

## Stinky

- **Mortal Wound** (debuff): React to `Mortal Wound` aura.

Suggested standing orders:
```
Mortal Wound on me -> away boss 10
```

## Festergut

- **Icc Festergut Group Position** (positioning): Encounter positioning (icc festergut group position).
- **Gas Spore** (debuff): React to `Gas Spore` aura.
- **Goo** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
Gas Spore on me -> away boss 12
in Goo -> out of it
```

## Rotface

- **Icc Rotface Tank Position** (positioning): Encounter positioning (icc rotface tank position).
- **Icc Rotface Group Position** (positioning): Encounter positioning (icc rotface group position).
- **Explosion** (cast_aoe): Boss cast AoE; move out during cast.
- **Vile Gas** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
Explosion cast -> out of it, then back
in Vile Gas -> out of it
```

## Volatile Ooze

- **Ooze** (add_switch): Priority add; switch targets to `Ooze` immediately.

Suggested standing orders:
```
Ooze appears -> target nearest Ooze
```

## Gas Cloud

- **Gas Cloud** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
in Gas Cloud -> out of it
```

## Professor Putricide

- **Puddle** (ground_hazard): Ground hazard (~8 yd); move out immediately.
- **Goo** (ground_hazard): Ground hazard; move out immediately.
- **Mutated Abomination** (add_switch): Priority add; switch targets to `Mutated Abomination` immediately.

Suggested standing orders:
```
in Puddle -> out of it
in Goo -> out of it
Mutated Abomination appears -> target nearest Mutated Abomination
```

## Prince Keleseth

- **Dark Nucleus** (add_switch): Priority add; switch targets to `Dark Nucleus` immediately.

Suggested standing orders:
```
Dark Nucleus appears -> target nearest Dark Nucleus
```

## Prince Valanar

- **Empowered Shock Vortex1** (cast_aoe): Boss cast AoE; move out during cast.
- **Flame** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
Empowered Shock Vortex1 cast -> out of it
in Flame -> out of it
```

## Blood Prince Council

- **Kinetic Bomb1** (add_switch): Priority add; switch targets to `Kinetic Bomb1` immediately.

Suggested standing orders:
```
Kinetic Bomb1 appears -> target nearest Kinetic Bomb1
```

## Blood-queen Lana'thel

- **Frenzied Bloodthirst** (debuff): React to `Frenzied Bloodthirst` aura.
- **Pact of the Darkfallen** (debuff): React to `Pact of the Darkfallen` aura.

Suggested standing orders:
```
Frenzied Bloodthirst on me -> away boss 10
Pact of the Darkfallen on me -> away boss 10
```

## Sister Svalna

- **Aether Shield** (debuff): React to `Aether Shield` aura.

Suggested standing orders:
```
Aether Shield on me -> away boss 10
```

## Valithria Dreamwalker

- **Dream State** (cast_aoe): Boss cast AoE (11 yd radius); move out during cast.
- **Kite Blistering Zombie** (kite): Kite `Blistering Zombie` away from the group.
- **Dream Portal** (add_switch): Priority add; switch targets to `Dream Portal` immediately.
- **Dream Cloud** (add_switch): Priority add; switch targets to `Dream Cloud` immediately.

Suggested standing orders:
```
Dream State cast -> out of it
Blistering Zombie targets me -> kite it
Dream Portal appears -> target nearest Dream Portal
Dream Cloud appears -> target nearest Dream Cloud
```

## Sindragosa

- **Tomb1** (add_switch): Priority add; switch targets to `Tomb1` immediately.
- **Frost Beacon** (debuff): React to `Frost Beacon` aura.
- **Blistering Cold** (cast_aoe): Boss cast AoE; move out during cast.
- **Unchained Magic** (debuff): React to `Unchained Magic` aura.
- **Chilled to the Bone** (debuff): React to `Chilled to the Bone` aura.
- **Ice Tomb** (facing): When `Ice Tomb` is active on Sindragosa, face away and stop attacking.

Suggested standing orders:
```
Tomb1 appears -> target nearest Tomb1
Frost Beacon on me -> away boss 10
Blistering Cold cast -> out of it
Unchained Magic on me -> away boss 10
Chilled to the Bone on me -> away boss 10
Ice Tomb on boss -> face away boss, then stop attacking
```

## The Lich King

- **Valithria Dreamwalker** (add_switch): Priority add; switch targets to `Valithria Dreamwalker` immediately.
- **Nitro Boosts** (ground_hazard): Ground hazard; move out immediately.

Suggested standing orders:
```
Valithria Dreamwalker appears -> target nearest Valithria Dreamwalker
in Nitro Boosts -> out of it
```

## Lich King

- **Lich King Spirit Bomb** (ground_hazard): Ground hazard (~20 yd); move out immediately.

Suggested standing orders:
```
in Lich King Spirit Bomb -> out of it
```

## Icecrown Citadel

- **Spike1** (add_switch): Priority add (`NPC_SPIKE1`); switch and burn immediately.
- **Spike2** (add_switch): Priority add (`NPC_SPIKE2`); switch and burn immediately.
- **Spike3** (add_switch): Priority add (`NPC_SPIKE3`); switch and burn immediately.
- **Choking Gas Bomb** (add_switch): Priority add (`NPC_CHOKING_GAS_BOMB`); switch and burn immediately.
- **Kinetic Bomb2** (add_switch): Priority add (`NPC_KINETIC_BOMB2`); switch and burn immediately.
- **Kinetic Bomb3** (add_switch): Priority add (`NPC_KINETIC_BOMB3`); switch and burn immediately.
- **Kinetic Bomb4** (add_switch): Priority add (`NPC_KINETIC_BOMB4`); switch and burn immediately.
- **Spirit Bomb** (add_switch): Priority add (`NPC_SPIRIT_BOMB`); switch and burn immediately.

Suggested standing orders:
```
Spike1 appears -> target nearest Spike1
Spike2 appears -> target nearest Spike2
Spike3 appears -> target nearest Spike3
Choking Gas Bomb appears -> target nearest Choking Gas Bomb
Kinetic Bomb2 appears -> target nearest Kinetic Bomb2
Kinetic Bomb3 appears -> target nearest Kinetic Bomb3
Kinetic Bomb4 appears -> target nearest Kinetic Bomb4
Spirit Bomb appears -> target nearest Spirit Bomb
```
