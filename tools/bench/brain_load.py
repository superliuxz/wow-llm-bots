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
     the urgent wakes' wait on their own (see docs/results.md).
"""

import heapq
import http.client
import re
import json
import os
import pathlib
import random
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from prompt_render import render_snapshot, render_tight  # noqa: E402

HOST, PORT, PATH = "127.0.0.1", 8090, "/v1/chat/completions"
URGENT = {"died", "combat", "new_attacker", "control", "hostile_player"}
RULES_MD = (pathlib.Path(__file__).resolve().parents[2] / "content" / "RULES.md").read_text()

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


RULES_MENU = RULES_MD

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


def menu_grammar(n, goal_optional=True, codes_only=False, loose=False, counts=False):
    """GBNF matching harness/src/plan.cc PlanGrammar()."""
    opt = " | ".join(f'"{i}"' for i in range(1, max(n, 1) + 1))
    cnt = " | ".join(f'"{i}"' for i in range(2, 30))
    tail = "" if codes_only else (" goal? say?" if goal_optional else ' "\ngoal: " goal say?')
    return "\n".join([
        f'root ::= "do: " opts "\\nexpect: " ("none" | ids) "\\nwatch: " watches '
        r'"\nlook: " looknum' + tail,
        'ids ::= opt (", " opt){0,4}',
        'opts ::= step (", " step){0,4}' if counts else 'opts ::= ids',
        'step ::= opt ("x" cnt)?',
        f"cnt ::= {cnt}",
        f"opt ::= {opt}",
        'watches ::= "none" | watch (", " watch){0,3}',
        f"watch ::= {WATCH}",
        "pct ::= [1-9] [0-9]?",
        "num ::= [1-9] [0-9]{0,2}",
        "looknum ::= [1-9] [0-9] [0-9]?",
        (r'goal ::= "\ngoal: " [^\n]+' if loose else r'goal ::= "\ngoal: " [^\n]{1,60}')
        if goal_optional else ("goal ::= [^\\n]+" if loose else "goal ::= [^\\n]{1,60}"),
        r'say ::= "\nsay: " [^\n]+' if loose else r'say ::= "\nsay: " [^\n]{1,80}',
    ])


def load(snap_dir, wakes_file, n_bots, seconds, rng):
    wakes = [json.loads(line) for line in open(wakes_file)]
    bots = sorted(p.name for p in pathlib.Path(snap_dir).iterdir() if p.is_dir())
    duration = max((w["t"] for w in wakes), default=1) or 1
    out = []
    # A run longer than the capture replays it again (the same timeline, one capture later).
    passes = int(seconds // duration) + 1
    for copy in range(n_bots):
        source = bots[copy % len(bots)]
        offset = 0 if copy < len(bots) else rng.uniform(0, duration)
        for w in wakes:
            if w["bot"] != source:
                continue
            for k in range(passes):
                t = (w["t"] + offset) % duration + k * duration
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


GOALS = {}  # copy -> the bot's current goal.
GOALS_LOCK = threading.Lock()
SLOT_LOCK = threading.Lock()
SLOT_BUSY = set()
LAST_SLOT = {}
NEXT_SLOT = [0]
TLS = threading.local()


def acquire_slot(copy, slots):
    if os.environ.get("PIN"):
        return copy % slots, False
    with SLOT_LOCK:
        pref = LAST_SLOT.get(copy)
        if pref is not None and pref < slots and pref not in SLOT_BUSY:
            SLOT_BUSY.add(pref)
            return pref, True
        for i in range(slots):
            s = (NEXT_SLOT[0] + i) % slots
            if s not in SLOT_BUSY:
                NEXT_SLOT[0] = (s + 1) % slots
                SLOT_BUSY.add(s)
                LAST_SLOT[copy] = s
                return s, True
        return copy % slots, False


def release_slot(slot, tracked):
    if tracked:
        with SLOT_LOCK:
            SLOT_BUSY.discard(slot)


def post_stream(body):
    """Streams /v1/chat/completions over a keep-alive HTTP connection; returns
    (text, usage, timings, first_line_elapsed_s)."""
    payload = json.dumps(body).encode()
    for attempt in range(2):
        conn = getattr(TLS, "conn", None)
        if conn is None:
            conn = http.client.HTTPConnection(HOST, PORT, timeout=600)
            TLS.conn = conn
        try:
            t0 = time.perf_counter()
            conn.request("POST", PATH, body=payload,
                         headers={"Content-Type": "application/json"})
            resp = conn.getresponse()
            if resp.status != 200:
                err = resp.read().decode("utf-8", "replace")
                raise RuntimeError(f"HTTP {resp.status}: {err[:200]}")
            text_parts = []
            usage, timings = {}, {}
            first_line_s = None
            while True:
                raw = resp.readline()
                if not raw:
                    break
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue
                data = line[5:].lstrip()
                if data == "[DONE]":
                    resp.read()  # Drain trailing CRLF so keep-alive stays clean.
                    break
                chunk = json.loads(data)
                choices = chunk.get("choices") or []
                if choices:
                    piece = (choices[0].get("delta") or {}).get("content")
                    if piece:
                        text_parts.append(piece)
                        if first_line_s is None and "\n" in piece:
                            first_line_s = time.perf_counter() - t0
                if chunk.get("usage"):
                    usage = chunk["usage"]
                if chunk.get("timings"):
                    timings = chunk["timings"]
            full_s = time.perf_counter() - t0
            return "".join(text_parts), usage, timings, (first_line_s or full_s)
        except (http.client.HTTPException, OSError):
            try:
                conn.close()
            except Exception:
                pass
            TLS.conn = None
            if attempt == 1:
                raise


HARNESS = pathlib.Path(__file__).resolve().parents[2] / "out" / "common" / "harness"
RULES_FILE = pathlib.Path(__file__).resolve().parents[2] / "content" / "RULES.md"
HARNESS_REQUESTS = {}
HARNESS_LOCK = threading.Lock()


INT64_KEYS = ("time_ms", "monotonic_us")


def harness_snapshot(snap_dir, bot, seq):
    """The snapshot file as the harness reads it. Captures from before the Glaze wire format
    (2026-11-17) wrote 64-bit numbers as strings, protobuf's JSON style: such a file gets a
    converted copy beside the captures (`<snap_dir>.harness/`)."""
    path = pathlib.Path(snap_dir) / bot / f"{seq}.json"
    text = path.read_text()
    if not any(f'"{k}": "' in text or f'"{k}":"' in text for k in INT64_KEYS):
        return path

    def fix(node):
        if isinstance(node, dict):
            return {k: int(v) if k in INT64_KEYS and isinstance(v, str) else fix(v)
                    for k, v in node.items()}
        if isinstance(node, list):
            return [fix(v) for v in node]
        return node

    out = pathlib.Path(f"{snap_dir}.harness") / bot / f"{seq}.json"
    if not out.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(fix(json.loads(text))))
    return out


def harness_request(snap_dir, wake):
    """PROMPT=harness: the request the harness itself would send for this wake-up (`harness
    decide --print-request`: its screen, RULES.md, grammar), so the numbers are the real
    brain's, not this prototype's. Cached per (bot, snapshot, reason); returns (body, kinds)."""
    key = (wake["source"], wake["seq"], wake["why"])
    with HARNESS_LOCK:
        if key in HARNESS_REQUESTS:
            return HARNESS_REQUESTS[key]
    path = harness_snapshot(snap_dir, wake["source"], wake["seq"])
    out = subprocess.run([str(HARNESS), "decide", str(path), "--print-request",
                          f"--rules={RULES_FILE}", f"--name={wake['source']}",
                          f"--reason={wake['why'].replace('_', ' ')}"],
                         capture_output=True, text=True, check=True).stdout
    got = json.loads(out)
    with HARNESS_LOCK:
        HARNESS_REQUESTS[key] = (got["body"], got["kinds"])
    return got["body"], got["kinds"]


def prepare_harness_requests(snap_dir, wakes):
    """Builds every wake-up's request before the clock starts (a process each)."""
    for wake in wakes:
        harness_request(snap_dir, {**wake, "source": wake["bot"]})


def call_harness_prompt(wake, snap_dir, slots):
    body, kinds = harness_request(snap_dir, wake)
    body = json.loads(json.dumps(body))  # Our copy: the goal and slot differ per call.
    with GOALS_LOCK:
        goal = GOALS.get(wake["copy"])
    if goal:
        # Where the harness writes it (prompt.cc): just before "Woken:".
        user = body["messages"][1]
        user["content"] = user["content"].replace("Woken: ", f"Current goal: {goal}.\nWoken: ", 1)
    body["temperature"] = float(os.environ.get("TEMP", 0.7))
    body["stream"] = True
    body["stream_options"] = {"include_usage": True}
    slot, tracked = acquire_slot(wake["copy"], slots)
    try:
        body["id_slot"] = slot
        content, usage, timings, first_line_s = post_stream(body)
    finally:
        release_slot(slot, tracked)
    plan = parse_plan(content)
    plan["_first_line_s"] = first_line_s
    plan["_valid"] = validate(plan, len(kinds), counts=True)
    if plan["_valid"]:
        plan["_est_s"] = estimate_seconds(plan, kinds)
    if plan.get("goal"):
        with GOALS_LOCK:
            GOALS[wake["copy"]] = plan["goal"]
    return plan, usage, timings


def call(wake, snap_dir, slots):
    if os.environ.get("PROMPT") == "harness":
        return call_harness_prompt(wake, snap_dir, slots)
    menu = os.environ.get("MODE") == "menu"
    goal_optional = menu and os.environ.get("GOAL_ONCE", "1") != "0"
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
    user_parts = [f"You are {wake['source']}."]
    if identity:
        user_parts.append(identity_text(wake["copy"], identity))
    wake_block = []
    if goal_optional:
        with GOALS_LOCK:
            goal = GOALS.get(wake["copy"])
        if goal:
            wake_block.append(f"Current goal: {goal}.")
    wake_block.append(f"Woken: {wake['why'].replace('_', ' ')}.")
    user_parts.append("\n".join(wake_block))
    user_parts.append(f"Your screen now:\n{screen}")

    slot, tracked = acquire_slot(wake["copy"], slots)
    try:
        body = {
            "messages": [
                {"role": "system", "content": rules},
                {"role": "user", "content": "\n\n".join(user_parts)},
            ],
            "max_tokens": int(os.environ.get("MAX_TOKENS", 150)),
            "temperature": float(os.environ.get("TEMP", 0.7)),
            "id_slot": slot,
            "cache_prompt": True,
            "stream": True,
            "stream_options": {"include_usage": True},
            "chat_template_kwargs": {"enable_thinking": False},
        }
        if menu and not os.environ.get("NO_GRAMMAR"):
            body["grammar"] = menu_grammar(len(options), goal_optional=goal_optional,
                                           codes_only=bool(os.environ.get("CODES_ONLY")),
                                           counts=counts)
        content, usage, timings, first_line_s = post_stream(body)
    finally:
        release_slot(slot, tracked)
    plan = parse_plan(content)
    plan["_first_line_s"] = first_line_s
    if menu:
        plan["_valid"] = validate(normalize(plan) if os.environ.get("NO_GRAMMAR") else plan,
                                  len(options), counts)
        if merged and plan["_valid"]:
            plan["_est_s"] = estimate_seconds(plan, kinds)
    if goal_optional and plan.get("goal"):
        with GOALS_LOCK:
            GOALS[wake["copy"]] = plan["goal"]
    return plan, usage, timings


WATCH_RE = re.compile(r"^(hp<[1-9]\d?|mana<[1-9]\d?|enemy_player<[1-9]\d{0,2}|attacked|whisper)$")


def estimate_seconds(plan, kinds):
    """How long until the bot wakes by itself: when its steps are done, or its `look`."""
    look = float(plan.get("look", "30") or 30)
    total = 0.0
    for part in plan["do"].split(","):
        number, _, times = part.strip().partition("x")
        kind = kinds[int(number) - 1]
        # The harness's names (OptionKindName) differ from this prototype's for two kinds.
        kind = {"keep doing what I'm doing": "keep", "go to town": "town"}.get(kind, kind)
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
    if os.environ.get("PROMPT") == "harness":
        prepare_harness_requests(snap_dir, wakes)
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
                plan, usage, timings, ok = str(e), None, {}, False
            done = time.perf_counter()
            with lock:
                busy[0] -= 1
                if busy[0] == 0:
                    busy_total[0] += done - busy_since[0]
                wait_s = start - t0 - wake["t"]
                first_s = plan.get("_first_line_s", done - start) if isinstance(plan, dict) else (done - start)
                results.append({**wake, "wait": wait_s, "think": done - start,
                                "first_latency": wait_s + first_s,
                                "latency": done - t0 - wake["t"], "ok": ok, "plan": plan,
                                # A call that failed (HTTP error, timeout) has no answer:
                                # it is left out of the latency and token figures.
                                "failed": not usage,
                                "prompt_tokens": (usage or {}).get("prompt_tokens"),
                                "read_tokens": timings.get("prompt_n"),
                                "reply_tokens": (usage or {}).get("completion_tokens")})

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
    failed = [r for r in results if r["failed"]]
    results = [r for r in results if not r["failed"]] or results
    lat = sorted(r["latency"] for r in results if not r["failed"])
    do_lat = sorted(r["first_latency"] for r in results if not r["failed"])
    look = [r["plan"].get("look_again_s") for r in results
            if r["ok"] and isinstance(r["plan"].get("look_again_s"), (int, float))]
    est = [r["plan"]["_est_s"] for r in results
           if r["ok"] and isinstance(r["plan"], dict) and "_est_s" in r["plan"]]
    counted = sum(1 for r in results if r["ok"] and isinstance(r["plan"], dict)
                  and re.search(r"x\d", r["plan"].get("do", "")))
    pct = lambda xs, q: xs[min(len(xs) - 1, int(q * len(xs)))] if xs else 0
    print(f"{n_bots} bots, {len(results)} wakes in {seconds:.0f} s "
          f"({len(results) / n_bots / seconds * 60:.2f} per bot per min): "
          f"wake->do p50 {pct(do_lat, 0.5):.2f}s p95 {pct(do_lat, 0.95):.2f}s | "
          f"wake->plan p50 {pct(lat, 0.5):.1f}s p95 {pct(lat, 0.95):.1f}s max {lat[-1] if lat else 0:.1f}s | "
          f"think p50 {statistics.median(r['think'] for r in results):.1f}s | "
          # The share of the run with at least one call in flight: a stand-in for GPU load
          # (it doesn't see how many calls run at once or what the GPU itself does).
          f"busy (a call in flight) {busy_total[0] / wall:.0%} | prompt "
          f"{statistics.mean(r['prompt_tokens'] or 0 for r in results):.0f} tok "
          f"(read again {statistics.mean(r['read_tokens'] or 0 for r in results):.0f}), plan "
          f"{statistics.mean(r['reply_tokens'] or 0 for r in results):.0f} tok | valid "
          f"{sum(r['ok'] for r in results)}/{len(results)} | brain's look_again_s median "
          f"{statistics.median(look) if look else '-'}"
          + (f" | {len(failed)} calls FAILED (left out)" if failed else ""))
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
