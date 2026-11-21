# Ahn'kahet: The Old Kingdom (AK)

<!-- DRAFT extracted from mod-playerbots (Dungeon/AK); hand-check before use. -->

## Elder Nadox

- **Guardian** (add_switch): Priority add; switch targets to `Guardian` immediately.

```
Guardian appears -> target nearest Guardian
```

## Jedoga Shadowseeker

- **Twilight Volunteer** (add_switch): Priority add; switch targets to `Twilight Volunteer` immediately.

```
Twilight Volunteer appears -> target nearest Twilight Volunteer
```

## Forgotten One

- **Shadow Crash** (ground_hazard): Ground hazard (~10 yd); move out immediately.

```
in Shadow Crash -> out of it
```
