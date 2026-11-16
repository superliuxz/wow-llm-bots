"""Prototype of a compact prompt rendering of a snapshot (for benchmarks only).

The snapshot itself stays complete; this only decides how it is written for the
LLM, which reads text tokens. Lossless for the brain except raw coordinates and
GUIDs (distance and compass direction stay):
- each unit or object is one short line in plain words, defaults left out
  (full health, in line of sight, idle, level height, medium size...);
- units that look the same at the same spot are one line listing every name;
- everything else is indented `key: value` lines, defaults left out.

The real renderer belongs in the C++ harness; this measures what it would save.
"""

import collections

SKIP_KEYS = {"guid", "position", "detail", "seq", "time_ms", "bot"}
DEFAULT_VALUES = {"IDLE", "LEVEL", "MEDIUM", "THREAT_NONE", "NORMAL", "NAMEPLATE"}


def word(value):
    return str(value).lower().replace("_", " ")


def where(rel):
    if not rel:
        return ""
    dist = rel.get("dist_yd")
    direction = rel.get("dir", "")
    parts = ["here" if direction == "HERE" or dist is None and not direction
             else f"{dist:.0f} yd {direction}" if dist is not None else direction]
    if rel.get("height") in ("ABOVE", "BELOW"):
        parts.append(word(rel["height"]))
    if not rel.get("in_los"):
        parts.append("out of sight")
    if rel.get("behind_it"):
        parts.append("you're behind it")
    if rel.get("facing_me"):
        parts.append("facing you")
    return ", ".join(parts)


def unit_look(u):
    """Everything about a unit except its name and position."""
    bits = []
    level = u.get("level")
    who = []
    if u.get("looks_like"):
        who.append(u["looks_like"])
    if (u.get("player") or {}).get("class"):
        who.append(u["player"]["class"].lower())
    if level or who:
        bits.append(" ".join(([f"level {level}"] if level else []) + who))
    for key in ("level_color", "kind", "reaction", "classification", "size"):
        if u.get(key) and u[key] not in DEFAULT_VALUES:
            bits.append(word(u[key]))
    for key in ("health_pct", "power_pct"):
        if key in u and u[key] < 100:
            bits.append(f"{key.split('_')[0]} {u[key]:.0f}%")
    activity = u.get("activity") or {}
    if activity.get("state") and activity["state"] not in DEFAULT_VALUES:
        bits.append(word(activity["state"]) + (f" {activity['with']}" if activity.get("with") else ""))
    for key, value in u.items():
        if key in SKIP_KEYS or key in ("name", "rel", "level", "looks_like", "player",
                                       "level_color", "kind", "reaction", "classification",
                                       "size", "health_pct", "power_pct", "activity"):
            continue
        if (isinstance(value, str) and value in DEFAULT_VALUES) or value in (False, None, "", [], {}):
            continue
        bits.append(f"{word(key)} {inline(value)}")
    return ", ".join(bits)


def inline(value):
    if isinstance(value, dict):
        return "(" + ", ".join(f"{word(k)} {inline(v)}" for k, v in value.items()
                               if k not in SKIP_KEYS) + ")"
    if isinstance(value, list):
        return "[" + "; ".join(inline(v) for v in value) + "]"
    if value is True:
        return "yes"
    if isinstance(value, float):
        return f"{value:g}"
    return word(value) if isinstance(value, str) and value.isupper() else str(value)


def render_things(items):
    """Units or objects: one line each; same look at the same spot -> one line."""
    groups = collections.OrderedDict()
    for item in items:
        key = (unit_look(item), where(item.get("rel")))
        groups.setdefault(key, []).append(item.get("name", "?"))
    lines = []
    for (look, place), names in groups.items():
        if len(names) == 1:
            lines.append(f"- {names[0]}: {look}; {place}")
        else:
            lines.append(f"- {len(names)}x {look}; {place}: {', '.join(names)}")
    return lines


def render(value, indent=""):
    lines = []
    for key, v in value.items():
        if key in SKIP_KEYS or v in (None, "", [], {}, False):
            continue
        if key in ("units", "objects") and isinstance(v, list):
            lines.append(f"{indent}{key}:")
            lines += [indent + "  " + line for line in render_things(v)]
        elif isinstance(v, dict):
            lines.append(f"{indent}{key}:")
            lines += render(v, indent + "  ")
        elif isinstance(v, list) and v and isinstance(v[0], dict):
            lines.append(f"{indent}{key}:")
            # Options are numbered: the brain answers with the number.
            lines += [f"{indent}  {i}. {inline(x)[1:-1]}" if key == "options"
                      else f"{indent}  - {inline(x)[1:-1]}" for i, x in enumerate(v, 1)]
        else:
            lines.append(f"{indent}{key}: {inline(v)}")
    return lines


def render_snapshot(snapshot):
    return "\n".join(render(snapshot))


# Tighter rendering: the base view in a few plain lines (everything else as above).
MELEE_YD = 10  # Facing and "behind it" only matter within melee range.


def self_line(s):
    u = s.get("unit", {})
    who = " ".join(x for x in (f"level {u.get('level', '?')}", word(s.get("gender", "")),
                               word((u.get("player") or {}).get("class", ""))) if x)
    bits = [f"{u.get('name', '?')}, {who}",
            f"health {s.get('health', 0)}/{s.get('health_max', 0)}"]
    if s.get("power_max"):
        bits.append(f"{word(s.get('power_type', 'power'))} {s.get('power', 0)}/{s['power_max']}")
    if s.get("xp_next"):
        bits.append(f"xp {s.get('xp', 0)}/{s['xp_next']}"
                    + (f" (rested {s['rested_xp']})" if s.get("rested_xp") else ""))
    if s.get("money"):
        bits.append(f"money {s['money']}c")
    flags = [word(k) for k in ("in_combat", "resting", "mounted", "swimming", "in_flight",
                               "camping", "drunk") if s.get(k)]
    if s.get("state", "ALIVE") != "ALIVE":
        flags.insert(0, word(s["state"]))
    return "; ".join(bits + flags)


def where_line(w):
    return ", ".join(x for x in (w.get("subzone"), w.get("zone")) if x) \
        + (f" ({w['map']})" if w.get("map") else "") \
        + ("; indoors" if w.get("indoors") else "") \
        + ("; in an instance" if w.get("in_instance") else "")


COARSE = False  # render_tight(coarse=True): distances in steps, units in a stable order.


def quant(yd):
    """A player can't judge yards by eye: 5-yd steps up close, 10-yd steps further out."""
    if not COARSE:
        return yd
    return round(yd / 5) * 5 if yd < 30 else round(yd / 10) * 10


def tight_where(rel):
    rel = dict(rel or {})
    if "dist_yd" in rel:
        rel["dist_yd"] = quant(rel["dist_yd"])
    if rel.get("dist_yd", 0) > MELEE_YD:
        rel.pop("behind_it", None)
        rel.pop("facing_me", None)
    return where(rel)


def tight_look(u):
    look = unit_look({k: v for k, v in u.items() if k != "dead"})  # `dead` is in activity.
    return look.replace(", usable yes", "").replace(", state ready", "")


def far_summary(units):
    """Nameless far units (model detail only): counts by look, with distances and directions."""
    groups = collections.OrderedDict()
    for u in units:
        groups.setdefault(tight_look(u), []).append(u.get("rel") or {})
    parts = []
    for look, rels in groups.items():
        d = sorted(quant(r.get("dist_yd", 0)) for r in rels)
        dirs = sorted({r.get("dir", "") for r in rels if r.get("dir")})
        span = f"{d[0]:.0f}" if len(d) == 1 or d[0] == d[-1] else f"{d[0]:.0f}-{d[-1]:.0f}"
        parts.append(f"{len(rels)}x {look} ({span} yd {'/'.join(dirs)})")
    return "; ".join(parts)


def tight_things(items):
    groups = collections.OrderedDict()
    for item in items:
        groups.setdefault((item.get("name", "?"), tight_look(item)), []).append(item.get("rel"))
    lines = []
    for (name, look), rels in groups.items():
        if len(rels) == 1:
            lines.append(f"- {name}: {look}; {tight_where(rels[0])}")
        else:
            d = sorted(quant((r or {}).get("dist_yd", 0)) for r in rels)
            dirs = "/".join(sorted({(r or {}).get("dir", "") for r in rels if r}))
            lines.append(f"- {len(rels)}x {name}: {look}; {d[0]:.0f}-{d[-1]:.0f} yd {dirs}")
    return lines


def render_tight(snapshot, coarse=False):
    global COARSE
    COARSE = coarse
    base = snapshot.get("base", {})
    lines = []
    if base.get("self"):
        lines.append("me: " + self_line(base["self"]))
    if base.get("where"):
        lines.append("where: " + where_line(base["where"]))
    if base.get("doing"):
        lines.append("doing: " + inline(base["doing"])[1:-1])
    nearby = base.get("nearby", {})
    named = [u for u in nearby.get("units", []) if u.get("name")]
    nameless = [u for u in nearby.get("units", []) if not u.get("name")]
    if coarse:  # A stable order, so an unchanged scene writes unchanged lines.
        named.sort(key=lambda u: (u["name"], (u.get("rel") or {}).get("dir", "")))
    if named:
        lines.append("around me:")
        lines += ["  " + line for line in tight_things(named)]
    if nameless:
        lines.append("far away: " + far_summary(nameless))
    if nearby.get("objects"):
        lines.append("objects:")
        lines += ["  " + line for line in tight_things(nearby["objects"])]
    rest = {k: v for k, v in base.items() if k not in ("self", "where", "doing", "nearby")}
    rest = {"base": rest} if rest else {}
    for key in ("ext", "events", "options"):
        if snapshot.get(key):
            rest[key] = snapshot[key]
    if rest:
        lines += render(rest)
    return "\n".join(lines)
