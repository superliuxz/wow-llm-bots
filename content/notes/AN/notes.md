# Azjol-Nerub (AN)

<!-- DRAFT extracted from mod-playerbots (Dungeon/AN); hand-check before use. -->

## Krik'thir the Gatewatcher

- **Web Wrap** (add_switch): Priority add; switch targets to `Web Wrap` immediately.
- **Krikthir** (add_switch): Priority add; switch targets to `Krikthir` immediately.

```
Web Wrap appears -> target nearest Web Wrap
Krikthir appears -> target nearest Krikthir
```

## Azjol-Nerub

- **Pound** (cast_aoe): Boss cast AoE (`SPELL_POUND_N`); move out during cast.

```
Pound cast -> out of it
```
