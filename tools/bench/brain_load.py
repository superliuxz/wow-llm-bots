"""Brain load replay: wakes from trigger_sim.py, played in real time against llama-server.

Each wake is a brain call: the bot's snapshot at that moment (compact prompt,
prompt_render.py), answered with a plan (docs/specs/triggers.md). Wakes queue by
urgency (combat and prompts before plan ends) for at most SLOTS calls in
flight, as the harness would. Measures how long a woken bot waits for its new
plan, and how busy the GPU is.

More bots than captured: each extra copy replays a captured bot's timeline
shifted by a random offset (wrapping around the capture).

Usage: python3 -I tools/bench/brain_load.py <snapshot_dir> <wakes.jsonl> [n_bots=50]
       [seconds=120]
Env: SLOTS (default 25), MAX_TOKENS (default 150), OUT=<file> (per-call JSON),
     MODE=menu: the brain may only pick from the bot's numbered options and the
     watch vocabulary, enforced by a grammar built per call (free text only in a
     short goal and an optional say line).
     RENDER=tight: prompt_render.render_tight (base view in a few plain lines).
     IDENTITY=<chars>: each bot gets its own identity and memory text of about that many
     characters (4 chars is about one token) after the shared rules, unique per bot, as
     var/bots/<name>/identity.md and its memories will be (1200 tokens is about 5000).
     PIN=1: a bot always uses its own slot (id_slot = bot number modulo SLOTS; SLOTS is the
     server's -np), so its identity and memory stay cached between its wake-ups. Without it
     slots are shared round-robin by the worker. WORKERS (default SLOTS) is how many calls
     run at once.
     TEMP=<t>: sampling temperature (default 0.7).
     GOAL_ONCE=1: the bot's current goal is in the prompt, and the brain writes a
     goal line only when it changes.
     MERGED=1: no separate options list; the screen's own entries carry the option numbers
     (prompt_render.render_tight(numbered=True)).
     LOOK_SAFETY=1 (with COUNTS): the prompt says `look` is only a safety net, usually 60-300 s.
     COUNTS=1: `do` may repeat an option: `3x10` is option 3, ten times (2-29). With MERGED=1
     the run also estimates how long each plan would really last (DURATION_S) and prints the
     time to the next wake-up it implies.
     NO_GRAMMAR=1: no grammar; the harness validates the reply afterwards (plan["_valid"]):
     option numbers that exist, watches from the vocabulary, a numeric look.
     CODES_ONLY=1: no goal or chat lines at all (a floor: the cost of the free text).
     BATCH_S=<s>: non-urgent wakes wait for the next multiple of <s> seconds and
     go to the GPU together (urgent ones go at once).
     SURPRISES=<n>: on top of the replayed wakes, every bot gets n urgent "combat" wakes per
     minute at random times (exponential gaps, so some come in bursts), each on the screen of
     a random wake of the same bot. The captured screens have no real fights, so this loads
     the brain with urgent wakes but not with a combat screen. The summary then also reports
     the urgent wakes' wait on their own (see docs/phase2-results.md).
"""

import heapq
import re
import json
import os
import pathlib
import random
import statistics
import sys
import threading
import time
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from prompt_render import render_snapshot, render_tight  # noqa: E402

URL = "http://127.0.0.1:8090/v1/chat/completions"
URGENT = {"died", "combat", "new_attacker", "control", "hostile_player"}

RULES = """\
You are a human player on a World of Warcraft 3.3.5a (Wrath of the Lich King) private server.
You play through the game client like everyone else, and only know what your screen, your
addons and your memory tell you. Never mention being an AI.

You are woken when something happens that your last plan didn't cover, or when it ends. You
see your screen now. Make a plan for the next while, like a real player: your hands carry it
out while you think about other things, and you're woken again if something unexpected
happens. Prefer plans that last a while (a few minutes of questing, a trip to town), not
single steps.

Reply in exactly this form, one short line each, nothing else:
goal: <what you're working towards>
do: <steps, separated by ;>
expect: <mobs or things this plan deals with, so they don't wake you>
watch: <extra things to wake you for, e.g. health below 40%; hostile player within 40 yd>
react: <what your hands do if surprised while you think>
look: <seconds until you look again anyway>
"""


RULES_MENU = """\
You are a human player on a World of Warcraft 3.3.5a (Wrath of the Lich King) private server.
You play through the game client like everyone else, and only know what your screen, your
addons and your memory tell you. Never mention being an AI.

You are woken when something happens that your plan didn't cover, or when it ends. You see
your screen and the numbered options you can take now. Make a plan for the next while: your
hands carry it out, and you're woken again if something unexpected happens.

Reply in exactly this form:
goal: <what you're working towards, a few words>
do: <option numbers, in order>
expect: <option numbers of the mobs this plan fights, or none>
watch: <none, or up to 4 of: hp<N, mana<N, enemy_player<YARDS, attacked, whisper>
look: <seconds until you look again anyway>
say: <optional: only if you want to say something in chat>
"""

WATCH = r'"hp<" pct | "mana<" pct | "enemy_player<" num | "attacked" | "whisper"'


def menu_options(snap):
    """A stand-in for Snapshot.options (snapshot steps 4-5): what's clickable now."""
    nearby = snap.get("base", {}).get("nearby", {})
    units = sorted((u for u in nearby.get("units", []) if u.get("name")),
                   key=lambda u: (u.get("rel") or {}).get("dist_yd", 999))
    where = lambda x: f"{(x.get('rel') or {}).get('dist_yd', 0):.0f} yd " \
                      f"{(x.get('rel') or {}).get('dir', '')}".strip()
    options, seen = [], set()
    for u in units:
        if u.get("kind") not in ("NPC", "CRITTER") or u["name"] in seen:
            continue
        if u.get("dead") and u.get("lootable"):
            options.append(f"Loot {u['name']} ({where(u)})")
        elif not u.get("dead") and u.get("reaction") in ("HOSTILE", "NEUTRAL"):
            options.append(f"Attack {u['name']} (level {u.get('level', '?')}, {where(u)})")
        elif not u.get("dead") and u.get("reaction") == "FRIENDLY":
            title = f", {u['subtitle']}" if u.get("subtitle") else ""
            options.append(f"Talk to {u['name']} ({where(u)}{title})")
        else:
            continue
        seen.add(u["name"])
        if len(options) >= 8:
            break
    objects = sorted((o for o in nearby.get("objects", []) if o.get("name") and o.get("usable")),
                     key=lambda o: (o.get("rel") or {}).get("dist_yd", 999))
    for o in objects:
        if o["name"] not in seen and len(options) < 12:
            verb = "Gather" if o.get("kind") in ("HERB", "ORE") else "Use"
            options.append(f"{verb} {o['name']} ({where(o)})")
            seen.add(o["name"])
    return options + ["Keep doing what I'm doing", "Explore somewhere new",
                      "Rest (eat and drink)", "Go to the nearest town"]


# How long one action takes, in seconds: stand-ins to estimate how long a plan lasts (a real
# measure needs the bot to play it). `keep` runs until the plan's own `look`.
DURATION_S = {"attack": 20, "talk": 8, "loot": 3, "gather": 10, "use": 8, "explore": 45,
              "rest": 30, "town": 60}


def menu_grammar(n, goal_optional=False, codes_only=False, loose=False, counts=False):
    """GBNF: only these option numbers, only the watch vocabulary."""
    opt = " | ".join(f'"{i}"' for i in range(1, n + 1))
    goal = r'("goal: " goal "\n")?' if goal_optional else r'"goal: " goal "\n"'
    if codes_only:
        goal = ""
    return "\n".join([
        f'root ::= {goal} "do: " opts "\\nexpect: " ("none" | ids) "\\nwatch: " watches '
        r'"\nlook: " num' + ("" if codes_only else " say?"),
        "goal ::= [^\\n]+" if loose else "goal ::= [^\\n]{1,60}",
        'ids ::= opt (", " opt){0,4}',
        'opts ::= step (", " step){0,4}' if counts else 'opts ::= ids',
        'step ::= opt ("x" cnt)?',
        "cnt ::= [2-9] | [1-2] [0-9]",
        f"opt ::= {opt}",
        'watches ::= "none" | watch (", " watch){0,3}',
        f"watch ::= {WATCH}",
        "pct ::= [1-9] [0-9]?",
        "num ::= [1-9] [0-9]{0,2}",
        r'say ::= "\nsay: " [^\n]+' if loose else r'say ::= "\nsay: " [^\n]{1,80}',
    ])


def load(snap_dir, wakes_file, n_bots, seconds, rng):
    wakes = [json.loads(line) for line in open(wakes_file)]
    bots = sorted(p.name for p in pathlib.Path(snap_dir).iterdir() if p.is_dir())
    duration = max((w["t"] for w in wakes), default=1) or 1
    out = []
    for copy in range(n_bots):
        source = bots[copy % len(bots)]
        offset = 0 if copy < len(bots) else rng.uniform(0, duration)
        for w in wakes:
            if w["bot"] != source:
                continue
            t = (w["t"] + offset) % duration
            if t < seconds:
                out.append({**w, "t": t, "copy": copy, "source": source})
    rate = float(os.environ.get("SURPRISES", 0)) / 60
    if rate:
        for copy in range(n_bots):
            source = bots[copy % len(bots)]
            mine = [w for w in out if w["copy"] == copy]
            if not mine:
                continue
            t = rng.expovariate(rate)
            while t < seconds:
                pick = rng.choice(mine)
                out.append({**pick, "t": t, "why": "combat", "surprise": True})
                t += rng.expovariate(rate)
    return sorted(out, key=lambda w: w["t"])


def snapshot_text(snap_dir, bot, seq):
    path = pathlib.Path(snap_dir) / bot / f"{seq}.json"
    return render_snapshot(json.loads(path.read_text()))


def snapshot(snap_dir, bot, seq):
    return json.loads((pathlib.Path(snap_dir) / bot / f"{seq}.json").read_text())


TRAITS = ["curious", "cautious", "reckless", "chatty", "quiet", "greedy", "generous", "stubborn",
          "patient", "impatient", "loyal", "sarcastic", "earnest", "lazy", "ambitious"]
PLACES = ["Orgrimmar", "Razor Hill", "the Barrens", "Sen'jin Village", "Thunder Bluff",
          "Undercity", "Ratchet", "Crossroads", "the Valley of Trials", "Durotar"]
THINGS = ["a boar that would not die", "a stranger who shared water", "a guild that fell apart",
          "a rare drop that never came", "a group that wiped twice", "a trainer who was rude",
          "a lost wallet", "a long walk at night", "a nice view from a hill"]


def identity_text(copy, chars):
    """Stand-in for var/bots/<name>/identity.md plus memories: unique per bot, about `chars`."""
    import random as _random
    rng = _random.Random(copy * 7919)
    traits = rng.sample(TRAITS, 3)
    lines = [f"Your identity (bot {copy}):",
             f"You are {', '.join(traits)}. You grew up near {rng.choice(PLACES)} and still "
             f"think of it as home. You play in the evenings and like "
             f"{rng.choice(['quests', 'herbs', 'chatting', 'exploring', 'grouping'])}.",
             "Your voice: short lines, a little slang from 2008, never formal.", "",
             "What you remember:"]
    while sum(len(x) + 1 for x in lines) < chars:
        lines.append(f"- {rng.choice(['Earlier today', 'Last night', 'Two days ago', 'A while ago'])}"
                     f" you thought about {rng.choice(THINGS)} near {rng.choice(PLACES)}; you felt "
                     f"{rng.choice(TRAITS)} about it and decided to "
                     f"{rng.choice(['keep going', 'wait and see', 'ask someone', 'let it go'])}.")
    return "\n".join(lines)


GOALS = {}  # copy -> the bot's current goal (GOAL_ONCE).
GOALS_LOCK = threading.Lock()


def call(wake, snap_dir, slots):
    menu = os.environ.get("MODE") == "menu"
    goal_once = menu and bool(os.environ.get("GOAL_ONCE"))
    snap = snapshot(snap_dir, wake["source"], wake["seq"])
    merged = menu and bool(os.environ.get("MERGED"))
    if merged:
        screen, kinds = render_tight(snap, numbered=True)
        options = [None] * len(kinds)
    else:
        screen = (render_tight if os.environ.get("RENDER") == "tight" else render_snapshot)(snap)
        options = menu_options(snap) if menu else []
        if menu:
            screen += "\noptions:\n" + "\n".join(f"  {i}. {o}" for i, o in enumerate(options, 1))
    rules = RULES_MENU if menu else RULES
    identity = int(os.environ.get("IDENTITY", 0))
    counts = menu and bool(os.environ.get("COUNTS"))
    if counts:
        rules = rules.replace(
            "do: <option numbers, in order>",
            "do: <option numbers, in order; add xN to repeat one N times, e.g. 3x4 does option 3 "
            "four times, one after another>").replace(
            "look: <seconds until you look again anyway>",
            "look: <seconds until you look again anyway: at least as long as your plan takes>")
    if counts and os.environ.get("LOOK_SAFETY"):
        rules = rules.replace(
            "look: <seconds until you look again anyway: at least as long as your plan takes>",
            "look: <a safety net only: you are woken at once if anything unexpected happens, so "
            "make it longer than your plan takes, usually 60 to 300 seconds>")
    if merged:
        rules = rules.replace(
            "your screen and the numbered options you can take now.",
            "your screen. Numbered entries are your options: picking a number attacks an enemy, "
            "talks to a friendly NPC, loots a corpse, or uses an object.")
    head = f"Woken: {wake['why'].replace('_', ' ')}."
    if goal_once:
        rules = rules.replace("goal: <what you're working towards, a few words>",
                              "goal: <only if your goal changed (or you have none yet): a few words>")
        with GOALS_LOCK:
            goal = GOALS.get(wake["copy"])
        head = (f"Your goal: {goal}." if goal else "You have no goal yet.") + " " + head
    body = {
        "messages": [
            {"role": "system", "content": rules + f"\nYou are {wake['source']}."
                                          + (("\n\n" + identity_text(wake["copy"], identity))
                                             if identity else "")},
            {"role": "user", "content": f"{head}\n\nYour screen now:\n{screen}"},
        ],
        "max_tokens": int(os.environ.get("MAX_TOKENS", 150)),
        "temperature": float(os.environ.get("TEMP", 0.7)),
        "id_slot": wake["copy"] % slots if os.environ.get("PIN") else wake["worker"] % slots,
        "cache_prompt": True,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    if menu and not os.environ.get("NO_GRAMMAR"):
        body["grammar"] = menu_grammar(len(options), goal_optional=goal_once,
                                       codes_only=bool(os.environ.get("CODES_ONLY")),
                                       counts=counts)
    req = urllib.request.Request(URL, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        out = json.load(r)
    plan = parse_plan(out["choices"][0]["message"]["content"])
    if menu:
        plan["_valid"] = validate(normalize(plan) if os.environ.get("NO_GRAMMAR") else plan,
                                  len(options), counts)
        if merged and plan["_valid"]:
            plan["_est_s"] = estimate_seconds(plan, kinds)
    if goal_once and plan.get("goal"):
        with GOALS_LOCK:
            GOALS[wake["copy"]] = plan["goal"]
    return plan, out["usage"], out.get("timings", {})


WATCH_RE = re.compile(r"^(hp<[1-9]\d?|mana<[1-9]\d?|enemy_player<[1-9]\d{0,2}|attacked|whisper)$")


def estimate_seconds(plan, kinds):
    """How long until the bot wakes by itself: when its steps are done, or its `look`."""
    look = float(plan.get("look", "30") or 30)
    total = 0.0
    for part in plan["do"].split(","):
        number, _, times = part.strip().partition("x")
        kind = kinds[int(number) - 1]
        total += (look if kind == "keep" else DURATION_S[kind]) * (int(times) if times else 1)
    return min(total, look)


def normalize(plan):
    """Forgiving reading of a plan the model wrote freely: `[5]` is 5, `hp20` is `hp<20`."""
    for key in ("do", "expect"):
        if key in plan:
            plan[key] = re.sub(r"\s*[x\u00d7]\s*", "x", re.sub(r"[\[\]()]", "", plan[key])).strip()
    if "watch" in plan:
        plan["watch"] = ", ".join(
            re.sub(r"^(hp|mana|enemy_player)\s*<?\s*(\d+)\s*(%|yd)?$", r"\1<\2", x.strip())
            for x in re.sub(r"[\[\]()]", "", plan["watch"]).split(","))
    if "look" in plan:
        m = re.match(r"\d+", plan["look"].strip())
        plan["look"] = m.group(0) if m else plan["look"]
    return plan


STEP_RE = re.compile(r"^(\d+)(?:x(\d+))?$")


def validate(plan, n_options, counts=False):
    """What the grammar guarantees, checked after the fact (invalid parts would be ignored)."""
    def numbers(text, with_counts=False):
        parts = [x.strip() for x in text.split(",")]
        for x in parts:
            m = STEP_RE.match(x)
            if not m or not 1 <= int(m.group(1)) <= n_options:
                return False
            if m.group(2) and not (with_counts and 2 <= int(m.group(2)) <= 29):
                return False
        return 0 < len(parts) <= 5
    try:
        return (numbers(plan["do"], counts)
                and (plan.get("expect", "none") == "none" or numbers(plan["expect"]))
                and (plan.get("watch", "none") == "none"
                     or all(WATCH_RE.match(x.strip()) for x in plan["watch"].split(",")))
                and plan.get("look", "").strip().isdigit())
    except KeyError:
        return False


def parse_plan(text):
    """The plan's `key: value` lines (plain text: fewer tokens than JSON, no grammar)."""
    plan = {}
    for line in text.splitlines():
        key, sep, value = line.partition(":")
        if sep and key.strip().lower() in ("goal", "do", "expect", "watch", "react", "look",
                                           "say"):
            plan[key.strip().lower()] = value.strip()
    if "look" in plan:
        try:
            plan["look_again_s"] = float(plan["look"].split()[0])
        except ValueError:
            pass
    return plan


def main():
    snap_dir, wakes_file = sys.argv[1], sys.argv[2]
    n_bots = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 120
    slots = int(os.environ.get("SLOTS", 25))
    workers = int(os.environ.get("WORKERS", slots))
    wakes = load(snap_dir, wakes_file, n_bots, seconds, random.Random(2))
    queue, lock, cv = [], threading.Lock(), threading.Condition(threading.Lock())
    results, busy = [], [0]
    busy_since, busy_total = [None], [0.0]
    t0 = time.perf_counter()

    def worker(number):
        while True:
            with cv:
                while not queue:
                    cv.wait()
                _, _, wake = heapq.heappop(queue)
            if wake is None:
                return
            with lock:
                if busy[0] == 0:
                    busy_since[0] = time.perf_counter()
                busy[0] += 1
            start = time.perf_counter()
            try:
                plan, usage, timings = call({**wake, "worker": number}, snap_dir, slots)
                ok = plan.get("_valid", "do" in plan)
            except Exception as e:  # Report, don't stop the replay.
                plan, usage, timings, ok = str(e), {}, {}, False
            done = time.perf_counter()
            with lock:
                busy[0] -= 1
                if busy[0] == 0:
                    busy_total[0] += done - busy_since[0]
                results.append({**wake, "wait": start - t0 - wake["t"], "think": done - start,
                                "latency": done - t0 - wake["t"], "ok": ok, "plan": plan,
                                "prompt_tokens": usage.get("prompt_tokens"),
                                "read_tokens": timings.get("prompt_n"),
                                "reply_tokens": usage.get("completion_tokens")})

    threads = [threading.Thread(target=worker, args=(n,)) for n in range(workers)]
    for t in threads:
        t.start()
    batch_s = float(os.environ.get("BATCH_S", 0))
    held = []  # Non-urgent wakes waiting for the next batch (BATCH_S).
    stop_flusher = threading.Event()

    def flush():
        with cv:
            for item in held:
                heapq.heappush(queue, item)
            held.clear()
            cv.notify_all()

    def flusher():
        while not stop_flusher.wait(batch_s - (time.perf_counter() - t0) % batch_s):
            flush()

    if batch_s:
        threading.Thread(target=flusher, daemon=True).start()
    for n, wake in enumerate(wakes):
        delay = wake["t"] - (time.perf_counter() - t0)
        if delay > 0:
            time.sleep(delay)
        item = (0 if wake["why"] in URGENT else 1, n, wake)
        with cv:
            if batch_s and item[0] == 1:
                held.append(item)
            else:
                heapq.heappush(queue, item)
                cv.notify()
    stop_flusher.set()
    flush()
    with cv:
        for n in range(workers):
            heapq.heappush(queue, (2, len(wakes) + n, None))
        cv.notify_all()
    for t in threads:
        t.join()
    wall = time.perf_counter() - t0
    lat = sorted(r["latency"] for r in results)
    look = [r["plan"].get("look_again_s") for r in results
            if r["ok"] and isinstance(r["plan"].get("look_again_s"), (int, float))]
    est = [r["plan"]["_est_s"] for r in results
           if r["ok"] and isinstance(r["plan"], dict) and "_est_s" in r["plan"]]
    counted = sum(1 for r in results if r["ok"] and isinstance(r["plan"], dict)
                  and re.search(r"x\d", r["plan"].get("do", "")))
    pct = lambda xs, q: xs[min(len(xs) - 1, int(q * len(xs)))] if xs else 0
    print(f"{n_bots} bots, {len(results)} wakes in {seconds:.0f} s "
          f"({len(results) / n_bots / seconds * 60:.2f} per bot per min): "
          f"wake->plan p50 {pct(lat, 0.5):.1f}s p95 {pct(lat, 0.95):.1f}s max {lat[-1] if lat else 0:.1f}s | "
          f"think p50 {statistics.median(r['think'] for r in results):.1f}s | "
          f"GPU busy {busy_total[0] / wall:.0%} | prompt "
          f"{statistics.mean(r['prompt_tokens'] or 0 for r in results):.0f} tok "
          f"(read again {statistics.mean(r['read_tokens'] or 0 for r in results):.0f}), plan "
          f"{statistics.mean(r['reply_tokens'] or 0 for r in results):.0f} tok | valid "
          f"{sum(r['ok'] for r in results)}/{len(results)} | brain's look_again_s median "
          f"{statistics.median(look) if look else '-'}")
    urgent = sorted(r["latency"] for r in results if r["why"] in URGENT)
    if os.environ.get("SURPRISES") and urgent:
        print(f"  urgent wakes ({len(urgent)}, {len(urgent) / n_bots / seconds * 60:.1f} per bot per min): "
              f"wake->plan p50 {pct(urgent, 0.5):.1f}s p95 {pct(urgent, 0.95):.1f}s max {urgent[-1]:.1f}s")
    if est:
        print(f"  plans with a count: {counted / len(results):.0%} | estimated time to the next "
              f"wake-up (steps done or look, whichever first): median {statistics.median(est):.0f}s, "
              f"mean {statistics.mean(est):.0f}s -> {60 / statistics.mean(est):.1f} wake-ups per bot "
              f"per minute")
    if os.environ.get("OUT"):
        pathlib.Path(os.environ["OUT"]).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
