"""Decode benchmark: why is writing the plan slow when many bots write at once?

N concurrent streams each write exactly TOKENS tokens (ignore_eos) after a real wake-up
prompt, so only the decoding speed is measured, with request-side variants:
  default   temperature 0.7 and the server's default sampler chain
  greedy    temperature 0 (no sampling randomness)
  topk1     top_k 1, and no other samplers
  grammar   the menu grammar (forces valid plans; may stop early, so TOKENS is a cap)
  loose     the same, but free text is `[^\\n]+` (no length counters; max_tokens caps it)
  codes     the same, with no free text at all

Prints total tokens/s, the time per step (all streams advance together), and, while it runs,
the GPU utilization and the server's CPU use, to tell a GPU limit from a CPU limit.

Usage: python3 -I tools/bench/decode_bench.py <snapshot_dir> [streams=25] [tokens=48]
Env: VARIANTS=default,greedy,topk1,grammar
"""

import json
import os
import pathlib
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import brain_load as b  # noqa: E402
from prompt_render import render_tight  # noqa: E402

BOTS = ["Loada", "Loadb", "Loadc", "Loadd", "Loade", "Loadf", "Loadg", "Loadh", "Loadi", "Loadj"]


def request(snap_dir, i, variant, tokens):
    bot = BOTS[i % len(BOTS)]
    snap = b.snapshot(snap_dir, bot, 100 + i)
    options = b.menu_options(snap)
    screen = render_tight(snap) + "\noptions:\n" + "\n".join(
        f"  {n}. {o}" for n, o in enumerate(options, 1))
    body = {
        "messages": [{"role": "system", "content": b.RULES_MENU + f"\nYou are {bot}."},
                     {"role": "user", "content": f"Woken: plan done.\n\nYour screen now:\n{screen}"}],
        "max_tokens": tokens, "id_slot": i, "cache_prompt": True,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    if variant == "default":
        body |= {"temperature": 0.7, "ignore_eos": True}
    elif variant == "greedy":
        body |= {"temperature": 0, "ignore_eos": True}
    elif variant == "topk1":
        body |= {"temperature": 0.7, "top_k": 1, "top_p": 1.0, "min_p": 0.0,
                 "samplers": ["top_k", "temperature"], "ignore_eos": True}
    elif variant == "grammar":
        body |= {"temperature": 0.7, "grammar": b.menu_grammar(len(options))}
    elif variant == "loose":
        body |= {"temperature": 0.7, "grammar": b.menu_grammar(len(options), loose=True)}
    elif variant == "codes":
        body |= {"temperature": 0.7, "grammar": b.menu_grammar(len(options), codes_only=True)}
    return body


def run(snap_dir, streams, tokens, variant):
    results = [None] * streams

    def one(i):
        req = urllib.request.Request(b.URL, json.dumps(request(snap_dir, i, variant, tokens)).encode(),
                                     {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=600) as r:
            out = json.load(r)
        results[i] = (out["usage"]["completion_tokens"], out["timings"]["predicted_ms"],
                      out["timings"]["prompt_ms"], out["timings"]["prompt_n"])

    # Warm the slots (prompt read once) so the timed run is decoding only.
    for i in range(streams):
        one_body = request(snap_dir, i, variant, 1)
        urllib.request.urlopen(urllib.request.Request(
            b.URL, json.dumps(one_body).encode(), {"Content-Type": "application/json"})).read()

    gpu = []
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            out = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,power.draw",
                                  "--format=csv,noheader,nounits"], capture_output=True, text=True)
            try:
                u, p = out.stdout.strip().split(",")
                gpu.append((float(u), float(p)))
            except ValueError:
                pass
            time.sleep(0.2)

    sampler = threading.Thread(target=sample)
    sampler.start()
    cpu0 = subprocess.run(["pgrep", "-x", "llama-server"], capture_output=True, text=True).stdout.split()
    t0 = time.perf_counter()
    threads = [threading.Thread(target=one, args=(i,)) for i in range(streams)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.perf_counter() - t0
    stop.set()
    sampler.join()
    total = sum(r[0] for r in results)
    longest = max(r[1] for r in results)
    steps = max(r[0] for r in results)
    util = sum(u for u, _ in gpu) / len(gpu) if gpu else 0
    power = sum(p for _, p in gpu) / len(gpu) if gpu else 0
    read = sum(r[3] for r in results)
    print(f"{variant:8s} {streams:3d} streams: {total} tokens in {wall:.2f}s = {total / wall:.0f} tok/s "
          f"| {longest / steps:.1f} ms per step | prompt read again: {read} tok, up to "
          f"{max(r[2] for r in results) / 1000:.1f}s | GPU util {util:.0f}% power {power:.0f} W")


def main():
    snap_dir = sys.argv[1]
    streams = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    tokens = int(sys.argv[3]) if len(sys.argv) > 3 else 48
    for variant in os.environ.get("VARIANTS", "default,greedy,topk1,grammar").split(","):
        run(snap_dir, streams, tokens, variant)


if __name__ == "__main__":
    main()
