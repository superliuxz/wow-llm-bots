"""Can a bot's conversation continue in its slot without re-reading what it already read?

Each bot keeps one slot. A wake-up appends to its conversation: the previous screen and the
brain's reply stay, and only what's new (the next screen) is added. If llama-server reuses the
slot's state, a wake-up only reads the new part. This checks it for this model (a hybrid, with
recurrent state: reuse isn't guaranteed), over several wake-ups of one bot.

Variants of what the next message holds:
  full   the whole next screen, as today (the conversation grows)
  delta  only what changed since the previous screen (see delta_text)

Prints, per wake-up: prompt tokens in total and how many were read again.

Usage: python3 -I tools/bench/continuation_test.py <snapshot_dir> [bot=Loada] [wakes=6]
"""

import json
import pathlib
import sys
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import brain_load as b  # noqa: E402
from prompt_render import render_tight  # noqa: E402


def screen_lines(snap):
    return render_tight(snap, coarse=True).splitlines()


def delta_text(prev, cur):
    """Lines that are new or changed since the previous screen, and what disappeared."""
    old, new = set(prev), set(cur)
    added = [line for line in cur if line not in old]
    gone = [line.strip(" -") for line in prev if line not in new and line.startswith("  - ")]
    text = "Changed since your last look:\n" + "\n".join(added or ["(nothing)"])
    if gone:
        text += "\nGone: " + "; ".join(gone[:6])
    return text


def ask(messages, slot, grammar):
    body = {"messages": messages, "max_tokens": 80, "temperature": 0.7, "id_slot": slot,
            "cache_prompt": True, "grammar": grammar,
            "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(b.URL, json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    out = json.load(urllib.request.urlopen(req))
    return out["choices"][0]["message"]["content"], out["usage"]["prompt_tokens"], \
        out["timings"]["prompt_n"], out["timings"]["prompt_ms"]


def main():
    snap_dir = sys.argv[1]
    bot = sys.argv[2] if len(sys.argv) > 2 else "Loada"
    wakes = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    for variant, slot in (("full", 0), ("delta", 1)):
        messages = [{"role": "system", "content": b.RULES_MENU + f"\nYou are {bot}."}]
        prev = None
        print(f"== {variant}")
        for k in range(wakes if variant == "delta" else min(wakes, 5)):  # "full" outgrows a slot.
            snap = b.snapshot(snap_dir, bot, 20 + k * 25)
            options = b.menu_options(snap)
            opts = "\noptions:\n" + "\n".join(f"  {i}. {o}" for i, o in enumerate(options, 1))
            cur = screen_lines(snap)
            if variant == "full" or prev is None:
                content = "Your screen now:\n" + "\n".join(cur) + opts
            else:
                content = delta_text(prev, cur) + opts
            messages.append({"role": "user", "content": content})
            reply, total, read, ms = ask(messages, slot, b.menu_grammar(len(options)))
            print(f"  wake {k + 1}: prompt {total:5d} tok, read again {read:5d} ({ms:.0f} ms)")
            messages.append({"role": "assistant", "content": reply})
            prev = cur


if __name__ == "__main__":
    main()
