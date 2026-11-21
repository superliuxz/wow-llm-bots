# Black Temple (BT)

<!-- Draft boss notes extracted from mod-playerbots (Raid/BT). -->

## High Warlord Naj'entus

- **Misdirection** (debuff): React to `Misdirection` aura.
- **High Warlord Naj'entus Boss Engaged by Tanks** (positioning): Encounter positioning (high warlord naj'entus boss engaged by tanks).
- **High Warlord Naj'entus Casts Needle Spines** (positioning): Boss disperses into sparks; move to safe disperse position.
- **Imp** (add_switch): Priority add; switch targets to `Imp` immediately.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Disperse cast -> away boss 15
Imp appears -> target nearest Imp
```

## Supremus

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Snare Self** (debuff): Debuff; run 10 yd away from Supremus.
- **Kite Supremus** (kite): Kite `Supremus` away from the group.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Snare Self on me -> away boss 10
Supremus targets me -> kite it
```

## Shade of Akama

- **Ashtongue Channeler** (add_switch): Priority add; switch targets to `Ashtongue Channeler` immediately.

Suggested standing orders:
```
Ashtongue Channeler appears -> target nearest Ashtongue Channeler
```

## Teron Gorefiend

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Teron Gorefiend Boss Engaged by Tanks** (positioning): Encounter positioning (teron gorefiend boss engaged by tanks).
- **Teron Gorefiend Boss Engaged by Ranged** (positioning): Encounter positioning (teron gorefiend boss engaged by ranged).
- **Shadow of Death** (cast_aoe): Boss cast AoE (15 yd radius); move out during cast.
- **Shadowy Construct** (add_switch): Priority add; switch targets to `Shadowy Construct` immediately.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Shadow of Death cast -> out of it
Shadowy Construct appears -> target nearest Shadowy Construct
```

## Gurtogg Bloodboil

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Boss Fel Rage** (debuff): React to `Boss Fel Rage` aura.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Boss Fel Rage on me -> away boss 10
```

## Reliquary of the Lost

- **Misdirection** (debuff): React to `Misdirection` aura.

Suggested standing orders:
```
Misdirection on me -> away boss 10
```

## Essence of Suffering

- **Tree of Life** (debuff): React to `Tree of Life` aura.

Suggested standing orders:
```
Tree of Life on me -> away boss 10
```

## Essence of Desire

- **Rune Shield** (debuff): React to `Rune Shield` aura.

Suggested standing orders:
```
Rune Shield on me -> away boss 10
```

## Mother Shahraz

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Fatal Attraction** (debuff): React to `Fatal Attraction` aura.
- **Mother Shahraz Tanks Are Positioning Boss** (positioning): Encounter positioning (mother shahraz tanks are positioning boss).

Suggested standing orders:
```
Misdirection on me -> away boss 10
Fatal Attraction on me -> away boss 10
```

## Gathios the Shatterer

- **Misdirection** (debuff): React to `Misdirection` aura.
- **Illidari Council Gathios Engaged by Main Tank** (positioning): Encounter positioning (illidari council gathios engaged by main tank).
- **Judgement** (debuff): React to `Judgement` aura.
- **Blessing of Protection** (debuff): React to `Blessing of Protection` aura.

Suggested standing orders:
```
Misdirection on me -> away boss 10
Judgement on me -> away boss 10
Blessing of Protection on me -> away boss 10
```

## Veras Darkshadow

- **Vanish** (debuff): React to `Vanish` aura.

Suggested standing orders:
```
Vanish on me -> away boss 10
```

## High Nethermancer Zerevor

- **Dampen Magic** (debuff): React to `Dampen Magic` aura.
- **Illidari Council Mage Tank Needs Dedicated Healer** (positioning): Encounter positioning (illidari council mage tank needs dedicated healer).
- **Illidari Council Zerevor Casts Dangerous Aoes** (positioning): Boss disperses into sparks; move to safe disperse position.

Suggested standing orders:
```
Dampen Magic on me -> away boss 10
Disperse cast -> away boss 15
```

## Illidan Stormrage

- **Flame** (ground_hazard): Ground hazard; move out immediately.
- **Parasite** (add_switch): Priority add; switch targets to `Parasite` immediately.
- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.
- **Fire** (ground_hazard): Ground hazard; move out immediately.
- **Dark Barrage** (debuff): React to `Dark Barrage` aura.
- **Caged** (debuff): React to `Caged` aura.
- **Shadow Demon** (add_switch): Priority add; switch targets to `Shadow Demon` immediately.
- **Flame of Azzinoth** (add_switch): Priority add; switch targets to `Flame of Azzinoth` immediately.
- **Demon Fire** (add_switch): Priority add; switch targets to `Demon Fire` immediately.

Suggested standing orders:
```
in Flame -> out of it
Parasite appears -> target nearest Parasite
Totem appears -> target nearest Totem
in Fire -> out of it
Dark Barrage on me -> away boss 10
Caged on me -> away boss 10
Shadow Demon appears -> target nearest Shadow Demon
Flame of Azzinoth appears -> target nearest Flame of Azzinoth
Demon Fire appears -> target nearest Demon Fire
```
