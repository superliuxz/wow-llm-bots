"""Think-cycle stress test: every bot's latest snapshot goes to the brain at once.

The simple design: the harness sends each bot's latest snapshot (plus what
happened since its last thought) to the LLM; the LLM answers with what to do
next. No rules in between. All bots are sent together, each on its own
llama-server slot so its fixed prompt prefix can stay cached.

Cycles run back to back, each with every bot's next captured snapshot, so the
cycle time is the shortest think period the GPU can sustain for this many bots.

Snapshots come from tools/bench/capture_snapshots.py (<dir>/<bot>/<seq>.json).
Needs llama-server on 127.0.0.1:8090 with at least n_bots slots
(tools/llm-server.sh).

Usage: python3 -I tools/bench/think_cycle.py <snapshot_dir> [n_bots=50] [cycles=3]
Env: MAX_TOKENS (default 120), THINK=1 (let the model reason first),
     RENDER=compact (prompt_render.py instead of JSON),
     OUT=<file> (write per-request results as JSON).
"""

import json
import os
import pathlib
import statistics
import sys
import threading
import time
import urllib.request

URL = "http://127.0.0.1:8090/v1/chat/completions"

# Stand-in for bots/RULES.md: identical for every bot, so it is the cached prefix.
RULES = """\
You are a human player on a World of Warcraft 3.3.5a (Wrath of the Lich King) private server.
You play through the game client like everyone else. You only know what your screen, your
addons (DBM, Questie, GearScore) and your own memory tell you. Never mention being an AI.

Every few seconds you get your screen as JSON: `base` is what is always visible (yourself,
your target, the units and objects around you with distance and compass direction, chat),
`ext` is what your addons and open panels show, `options` are the things you can click right
now, and `events` is what happened since your last look.

Decide what to do next like a real player would: pick a goal for the next while (a quest, a
trip to town, a group, a chat), and the next concrete step towards it. You may ignore things,
change your mind, chat, or just stand around if that's what your character would do. Keep
your own pace: you don't have to react to everything.

Rules: you never ninja loot, rage quit, grief, or sabotage a group; trolling stays in chat,
never slurs. You play 3.3.5a: no transmog, no LFR, no mythic+, no modern terms.

Reply with only a JSON object:
{"goal": "<what you're working towards, a few words>",
 "do": "<your next concrete step: an option number, or move/target/cast/use/loot/talk ...>",
 "say": null or {"channel": "say|party|guild|whisper|trade", "to": "<name or null>", "text": "..."},
 "look_again_s": <seconds until you want your next look, 1-30>}
"""

PERSONALITIES = [
    "chatty, helpful, plays casually after work",
    "quiet, efficient, wants to hit 80 fast",
    "explorer, easily distracted by new places",
    "social, likes grouping and guild chat",
    "achiever, min-maxes everything",
]


def load_snapshots(root, n_bots):
    bots = sorted(p for p in pathlib.Path(root).iterdir() if p.is_dir())[:n_bots]
    if os.environ.get("RENDER") == "compact":
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        from prompt_render import render_snapshot
        compact = lambda f: render_snapshot(json.loads(f.read_text()))
    else:
        compact = lambda f: json.dumps(json.loads(f.read_text()), separators=(",", ":"))
    return {b.name: [compact(f) for f in sorted(b.glob("*.json"), key=lambda f: int(f.stem))]
            for b in bots}


def make_request(i, bot, snapshot, last):
    identity = (f"You are {bot}. Personality: {PERSONALITIES[i % len(PERSONALITIES)]}.")
    previous = f"Your last decision: {last}" if last else "You just logged in."
    body = {
        "messages": [
            {"role": "system", "content": RULES + "\n" + identity},
            {"role": "user", "content": f"{previous}\n\nYour screen now:\n{snapshot}"},
        ],
        "max_tokens": int(os.environ.get("MAX_TOKENS", 120)),
        "temperature": 0.7,
        "id_slot": i,
        "cache_prompt": True,
        "chat_template_kwargs": {"enable_thinking": bool(os.environ.get("THINK"))},
    }
    if not os.environ.get("THINK"):
        body["response_format"] = {"type": "json_object"}
    return body


def call(i, bot, snapshot, last, results):
    body = json.dumps(make_request(i, bot, snapshot, last)).encode()
    req = urllib.request.Request(URL, data=body, headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            out = json.load(r)
        text = out["choices"][0]["message"]["content"]
        timings = out.get("timings", {})
        try:
            reply = json.loads(text)
            ok = isinstance(reply, dict) and "do" in reply
        except json.JSONDecodeError:
            reply, ok = text, False
        results[i] = {
            "bot": bot, "latency": time.perf_counter() - t0, "ok": ok, "reply": reply,
            "prompt_tokens": out["usage"]["prompt_tokens"],
            "prefilled": timings.get("prompt_n"),  # Tokens actually computed (not cached).
            "prompt_ms": timings.get("prompt_ms"),
            "generated": out["usage"]["completion_tokens"],
            "gen_ms": timings.get("predicted_ms"),
        }
    except Exception as e:  # Report, don't crash the cycle.
        results[i] = {"bot": bot, "latency": time.perf_counter() - t0, "ok": False,
                      "reply": f"ERROR {e}", "prompt_tokens": 0, "prefilled": 0, "generated": 0}


def cycle(snapshots, index, last):
    bots = list(snapshots)
    results = [None] * len(bots)
    threads = []
    for i, bot in enumerate(bots):
        frames = snapshots[bot]
        snap = frames[min(index, len(frames) - 1)]
        threads.append(threading.Thread(target=call, args=(i, bot, snap, last.get(bot), results)))
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return time.perf_counter() - t0, results


def main():
    root = sys.argv[1]
    n_bots = int(sys.argv[2]) if len(sys.argv) > 2 else 50
    cycles = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    snapshots = load_snapshots(root, n_bots)
    sizes = [len(f[0]) for f in snapshots.values()]
    print(f"{len(snapshots)} bots, snapshot JSON {statistics.mean(sizes) / 1024:.1f} KB avg")
    last, log = {}, []
    for c in range(cycles):
        wall, res = cycle(snapshots, c, last)
        lat = sorted(r["latency"] for r in res)
        prompt = sum(r["prompt_tokens"] for r in res)
        prefilled = sum(r["prefilled"] or 0 for r in res)
        generated = sum(r["generated"] for r in res)
        print(f"cycle {c + 1}: wall {wall:.1f}s | latency p50 {statistics.median(lat):.1f}s "
              f"max {lat[-1]:.1f}s | prompt {prompt / len(res):.0f} tok/bot, "
              f"computed {prefilled} of {prompt} ({prefilled / max(prompt, 1):.0%}) "
              f"-> {prefilled / wall:.0f} tok/s | reply {generated / len(res):.0f} tok/bot | "
              f"valid {sum(r['ok'] for r in res)}/{len(res)}")
        for r in res:
            if r["ok"]:
                last[r["bot"]] = json.dumps(r["reply"])
        log.append({"cycle": c + 1, "wall": wall, "results": res})
    print("\nsample replies (last cycle):")
    for r in res[:6]:
        print(f"  {r['bot']}: {json.dumps(r['reply'])[:220]}")
    if os.environ.get("OUT"):
        pathlib.Path(os.environ["OUT"]).write_text(json.dumps(log, indent=1))


if __name__ == "__main__":
    main()
