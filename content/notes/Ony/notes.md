# Onyxia's Lair (Ony)

<!-- DRAFT extracted from mod-playerbots (Raid/Ony); hand-check before use. -->

## Onyxia

- **Breath** (cast_aoe): Boss cast AoE; move out during cast.
- **Ony Fireball Splash Incoming** (cast_aoe): Boss cast AoE; move out during cast.
- **Whelp** (add_switch): Priority add; switch targets to `Whelp` immediately.

```
Breath cast -> out of it
Ony Fireball Splash Incoming cast -> out of it
Whelp appears -> target nearest Whelp
```
