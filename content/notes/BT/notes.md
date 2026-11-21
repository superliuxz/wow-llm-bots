# Black Temple (BT)

<!-- DRAFT extracted from mod-playerbots (Raid/BT); hand-check before use. -->

## High Warlord Naj'entus

- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **High Warlord Naj'entus Boss Engaged by Tanks** (positioning): Encounter positioning (high warlord naj'entus boss engaged by tanks).
- **High Warlord Naj'entus Casts Needle Spines** (positioning): Boss disperses into sparks; move to safe disperse position.
- **Imp** (add_switch): Priority add; switch targets to `Imp` immediately.

```
Disperse cast -> away boss 15
Imp appears -> target nearest Imp
```

## Supremus

- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **Snare Self** (debuff): Debuff; run 10 yd away from Supremus.
- **Kite Supremus** (kite): Kite `Supremus` away from the group.

```
Snare Self on me -> away boss 10
Supremus targets me -> kite it
```

## Shade of Akama

- **Ashtongue Channeler** (add_switch): Priority add; switch targets to `Ashtongue Channeler` immediately.

```
Ashtongue Channeler appears -> target nearest Ashtongue Channeler
```

## Teron Gorefiend

- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **Teron Gorefiend Boss Engaged by Tanks** (positioning): Encounter positioning (teron gorefiend boss engaged by tanks).
- **Teron Gorefiend Boss Engaged by Ranged** (positioning): Encounter positioning (teron gorefiend boss engaged by ranged).
- **Shadow of Death** (cast_aoe): Boss cast AoE (15 yd radius); move out during cast.
- **Shadowy Construct** (add_switch): Priority add; switch targets to `Shadowy Construct` immediately.

```
Shadow of Death cast -> out of it
Shadowy Construct appears -> target nearest Shadowy Construct
```

## Gurtogg Bloodboil

- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **Boss Fel Rage** (debuff): Aura `Boss Fel Rage` (me); gather/position per encounter script (hand-check).

## Reliquary of the Lost

- **Misdirection** (debuff): Aura `Misdirection` on boss; hand-check encounter reaction.

## Essence of Suffering

- **Tree of Life** (debuff): Aura `Tree of Life` on me; hand-check encounter reaction.

## Essence of Desire

- **Rune Shield** (debuff): Aura `Rune Shield` on me; hand-check encounter reaction.

## Mother Shahraz

- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **Fatal Attraction** (debuff): Aura `Fatal Attraction` (me); gather/position per encounter script (hand-check).
- **Mother Shahraz Tanks Are Positioning Boss** (positioning): Encounter positioning (mother shahraz tanks are positioning boss).

## Gathios the Shatterer

- **Misdirection** (debuff): Aura `Misdirection` on me; hand-check encounter reaction.
- **Illidari Council Gathios Engaged by Main Tank** (positioning): Encounter positioning (illidari council gathios engaged by main tank).
- **Judgement** (debuff): Aura `Judgement` on me; hand-check encounter reaction.
- **Blessing of Protection** (debuff): Aura `Blessing of Protection` on me; hand-check encounter reaction.

## Veras Darkshadow

- **Vanish** (debuff): Aura `Vanish` (me); gather/position per encounter script (hand-check).

## High Nethermancer Zerevor

- **Dampen Magic** (debuff): Aura `Dampen Magic` (me); gather/position per encounter script (hand-check).
- **Illidari Council Mage Tank Needs Dedicated Healer** (positioning): Encounter positioning (illidari council mage tank needs dedicated healer).
- **Illidari Council Zerevor Casts Dangerous Aoes** (positioning): Boss disperses into sparks; move to safe disperse position.

```
Disperse cast -> away boss 15
```

## Illidan Stormrage

- **Illidan Stormrage Boss Casts Flame Crash in Front of Main Tank** (positioning): Encounter positioning (illidan stormrage boss casts flame crash in front of main tank).
- **Parasite** (add_switch): Priority add; switch targets to `Parasite` immediately.
- **Totem** (add_switch): Priority add; switch targets to `Totem` immediately.
- **Demon Fire** (add_switch): Priority add; switch targets to `Demon Fire` immediately.
- **Illidan Stormrage Grate Is Safe from Flames** (positioning): Encounter positioning (illidan stormrage grate is safe from flames).
- **Dark Barrage** (debuff): Aura `Dark Barrage` on me; hand-check encounter reaction.
- **Caged** (debuff): Aura `Caged` on me; hand-check encounter reaction.
- **Shadow Demon** (add_switch): Priority add; switch targets to `Shadow Demon` immediately.
- **Flame of Azzinoth** (add_switch): Priority add; switch targets to `Flame of Azzinoth` immediately.

```
Parasite appears -> target nearest Parasite
Totem appears -> target nearest Totem
Demon Fire appears -> target nearest Demon Fire
Shadow Demon appears -> target nearest Shadow Demon
Flame of Azzinoth appears -> target nearest Flame of Azzinoth
```
