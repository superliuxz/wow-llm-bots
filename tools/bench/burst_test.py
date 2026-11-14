"""Burst test: N bots ask the local LLM for a decision at the same moment.

Measures wall time for the whole burst, per-request latency, and whether
every reply is valid JSON in the command schema. Run twice in a row to see
cold vs warm (per-slot prompt cache) behavior.

Usage: python3 -I tools/bench/burst_test.py [n_bots=25] [rounds=2]
"""

import json
import os
import statistics
import sys
import threading
import time
import urllib.request

URL = "http://127.0.0.1:8090/v1/chat/completions"

# Stand-in for bots/RULES.md: a long, identical prefix shared by every bot.
RULES = (
    "You are a human player on a World of Warcraft 3.3.5a (Wrath of the Lich King) "
    "private server. You play through the game client like everyone else. You can only "
    "do what a player can do: move, target, cast spells, use items, loot, and chat. "
    "You only know what your screen, your addons (DBM, Questie) and your own memory "
    "tell you. Decide like a real person: you may follow or ignore raid calls, you make "
    "mistakes when busy, and you never ninja loot, rage quit or sabotage the group. "
) * 12

SCHEMA = {
    "type": "object",
    "properties": {
        "action": {"type": "string", "enum": ["move", "cast", "target", "wait", "say"]},
        "detail": {"type": "string"},
        "say": {"type": ["string", "null"]},
    },
    "required": ["action", "detail", "say"],
    "additionalProperties": False,
}

CLASSES = ["warrior", "paladin", "hunter", "rogue", "priest",
           "death knight", "shaman", "mage", "warlock", "druid"]


def make_request(i):
    identity = (
        f"You are bot #{i}, a level 80 {CLASSES[i % len(CLASSES)]} named Bot{i}. "
        f"Personality: {'chatty' if i % 3 == 0 else 'quiet'}, "
        f"{'veteran raider' if i % 2 == 0 else 'new to raiding'}."
    )
    situation = (
        "Icecrown Citadel, Lich King phase 2. Your health 82%. DBM: 'Defile in 3s'. "
        "Raid warning from the raid leader Gorvak: 'Defile soon, spread out!'. "
        "The tank Grumbar is 20 yards north. You are standing in the middle of the raid. "
        "What do you do right now? Answer with one command as JSON: {\"action\": move|cast|target|wait|say, \"detail\": ..., \"say\": ... or null}."
    )
    return {
        "messages": [
            {"role": "system", "content": RULES + "\n\n" + identity},
            {"role": "user", "content": situation},
        ],
        "max_tokens": 60,
        "temperature": 0.7,
        "chat_template_kwargs": {"enable_thinking": False},
    } | ({} if os.environ.get("NO_SCHEMA") else
         {"response_format": {"type": "json_schema", "json_schema": {"name": "cmd", "schema": SCHEMA}}})


def call(i, results):
    body = json.dumps(make_request(i)).encode()
    req = urllib.request.Request(URL, data=body, headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            out = json.load(r)
        dt = time.perf_counter() - t0
        text = out["choices"][0]["message"]["content"]
        usage = out.get("usage", {})
        try:
            cmd = json.loads(text)
            ok = cmd.get("action") in SCHEMA["properties"]["action"]["enum"]
        except json.JSONDecodeError:
            cmd, ok = text, False
        results[i] = {"latency": dt, "ok": ok, "cmd": cmd,
                      "prompt_tokens": usage.get("prompt_tokens"),
                      "cached": (usage.get("prompt_tokens_details") or {}).get("cached_tokens"),
                      "completion_tokens": usage.get("completion_tokens")}
    except Exception as e:  # report, don't crash the burst
        results[i] = {"latency": time.perf_counter() - t0, "ok": False, "cmd": f"ERROR {e}"}


def burst(n):
    results = [None] * n
    threads = [threading.Thread(target=call, args=(i, results)) for i in range(n)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return time.perf_counter() - t0, results


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 25
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    for r in range(rounds):
        wall, res = burst(n)
        lat = sorted(x["latency"] for x in res)
        print(f"round {r + 1} ({'cold' if r == 0 else 'warm'}): {n} bots, wall {wall:.2f}s, "
              f"latency p50 {statistics.median(lat):.2f}s p95 {lat[int(0.95 * (n - 1))]:.2f}s "
              f"max {lat[-1]:.2f}s, valid {sum(x['ok'] for x in res)}/{n}, "
              f"prompt {res[0].get('prompt_tokens')} tok (cached {res[0].get('cached')}), "
              f"reply ~{statistics.mean(x.get('completion_tokens') or 0 for x in res):.0f} tok")
    print("\nsample replies (last round):")
    for x in res[:8]:
        print("  ", json.dumps(x["cmd"])[:160])


if __name__ == "__main__":
    main()
