# Onyxia's Lair (Ony)

<!-- Draft boss notes extracted from mod-playerbots (Raid/Ony). -->

## Onyxia

- **Breath** (cast_aoe): Boss cast AoE; move out during cast.
- **Fire** (ground_hazard): Ground hazard; move out immediately.
- **Whelp** (add_switch): Priority add; switch targets to `Whelp` immediately.

Suggested standing orders:
```
Breath cast -> out of it
in Fire -> out of it
Whelp appears -> target nearest Whelp
```
