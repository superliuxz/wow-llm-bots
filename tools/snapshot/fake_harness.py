"""Minimal stand-in for the harness, for testing mod-llm-brain's socket.

Connects to the brain socket and, for a few seconds, either prints what the server sends or
measures how it arrives. Messages are length-prefixed BEVE frames (4-byte big-endian length, then
the payload), as in common/net/frame_codec.h.

Dev tool only (production is C++). Needs `tools/wlb build common --no-tests` once for wire_py:
  python3 -I tools/snapshot/fake_harness.py [seconds=5] [message ...]
  python3 -I tools/snapshot/fake_harness.py --stats [--json] [seconds=10]
  python3 -I tools/snapshot/fake_harness.py --choose [--json] [--tail=S] [seconds=30] Attack Loot
  python3 -I tools/snapshot/fake_harness.py --whisper [--json] [seconds=30] FROM TO TEXT REPLY
"""

import collections
import json
import pathlib
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from frame_reader import FrameReader  # noqa: E402
import wire_py  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SOCK = str(ROOT / "out" / "server" / "run" / "brain.sock")


def describe(payload):
    pretty = wire_py.module_message_to_json(payload, pretty=True)
    if pretty is not None:
        return pretty
    return f"[{len(payload)} bytes] {payload[:32].hex()}"


def percentile(sorted_values, q):
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))] if sorted_values else 0


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.2)
    return s


def measure_delivery(seconds, warmup=2.0):
    """Receives snapshots for `seconds` and measures how they arrive (from the snapshots' own
    timestamps, monotonic_us: this machine's monotonic clock, the same in every process)."""
    s = connect()
    reader = FrameReader()
    ages, arrivals = [], []
    per_bot = collections.defaultdict(list)
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            break
        received_ms = time.monotonic_ns() / 1e6
        for payload in reader.feed(chunk):
            message = wire_py.decode_module_message(payload)
            if not message or "snapshot" not in message:
                continue
            snap = message["snapshot"]
            built_ms = snap.get("monotonic_us", 0) / 1000
            ages.append(received_ms - built_ms)
            arrivals.append(received_ms / 1000)
            per_bot[snap.get("bot", "")].append(built_ms)
    s.close()
    if not ages:
        return {"error": "no snapshots received"}

    def bunching(times):
        window, most = collections.deque(), 0
        for t in times:
            window.append(t)
            while t - window[0] > 0.010:
                window.popleft()
            most = max(most, len(window))
        return most

    first = arrivals[0]
    out = {"snapshots": len(ages), "bots": len(per_bot), "per_s": len(ages) / seconds,
           "warmup_s": warmup,
           "first_bunching": bunching([t for t in arrivals if t - first < warmup])}
    steady = [i for i, t in enumerate(arrivals) if t - first >= warmup]
    if steady:
        steady_ages = sorted(ages[i] for i in steady)
        steady_from_ms = arrivals[steady[0]] * 1000
        gaps = sorted(b - a for times in per_bot.values()
                      for a, b in zip(times, times[1:]) if a >= steady_from_ms - 1500)
        out["steady"] = {
            "n": len(steady),
            "age_p50_ms": percentile(steady_ages, .5),
            "age_p95_ms": percentile(steady_ages, .95),
            "age_max_ms": steady_ages[-1],
            "spacing_p5_ms": percentile(gaps, .05), "spacing_p50_ms": percentile(gaps, .5),
            "spacing_p95_ms": percentile(gaps, .95),
            "bunching": bunching([arrivals[i] for i in steady]),
        }
    return out


def run_stats(seconds, as_json=False, warmup=2.0):
    r = measure_delivery(seconds, warmup)
    if as_json:
        print(json.dumps(r))
        return
    if "error" in r:
        print(r["error"])
        return
    print(f"{r['snapshots']} snapshots from {r['bots']} bots in {seconds:.0f}s "
          f"({r['per_s']:.1f}/s)")
    print(f"first {warmup:.0f} s (every bot's first snapshot): up to "
          f"{r['first_bunching']} within 10 ms")
    st = r.get("steady")
    if not st:
        print("(no steady state: run longer)")
        return
    print(f"steady state ({st['n']} snapshots):")
    print(f"  age on arrival (built -> received): p50 {st['age_p50_ms']:.1f} ms, "
          f"p95 {st['age_p95_ms']:.1f} ms, max {st['age_max_ms']:.1f} ms")
    print(f"  spacing of one bot's snapshots: p5 {st['spacing_p5_ms']:.0f} ms, "
          f"p50 {st['spacing_p50_ms']:.0f} ms, p95 {st['spacing_p95_ms']:.0f} ms")
    print(f"  bunching: up to {st['bunching']} snapshots arrived within 10 ms")


def run_print(seconds, to_send):
    s = connect()
    for message in to_send:
        payload = message.encode()
        s.sendall(len(payload).to_bytes(4, "big") + payload)
        print(f">> {message}")
    reader = FrameReader()
    received = 0
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            print("server closed the connection")
            break
        for payload in reader.feed(chunk):
            received += 1
            print(f"<< {describe(payload)}")
    s.close()
    print(f"received {received} messages in {seconds:.0f}s")


def send_frame(s, payload):
    s.sendall(len(payload).to_bytes(4, "big") + payload)


def play_script(seconds, words, echo=print, tail_s=None):
    s = connect()
    reader = FrameReader()
    words = list(words)
    out = {"snapshots": [], "events": [], "commands": [], "never_offered": [], "closed": False}
    start = time.time()
    next_at = 0.0
    sent = 0
    stop_at = start + seconds
    while time.time() < stop_at:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            out["closed"] = True
            echo("server closed the connection")
            break
        for payload in reader.feed(chunk):
            message = wire_py.decode_module_message(payload)
            if not message:
                continue
            t = round(time.time() - start, 3)
            if "event" in message:
                imm = message["event"]
                event = imm.get("event", {})
                out["events"].append({"t": t, "event": event})
                echo(f"<< event for {imm.get('bot', '')}: {event}")
                continue
            snap = message.get("snapshot", {})
            me = snap.get("base", {}).get("self", {})
            options = [o.get("text", "") for o in snap.get("options", [])]
            seq = snap.get("seq", 0)
            bot = snap.get("bot", "")
            hp = me.get("health", 0)
            hp_max = me.get("health_max", 0)
            in_combat = bool(me.get("in_combat", False))
            pos = snap.get("base", {}).get("where", {}).get("position", {})
            out["snapshots"].append({"t": t, "seq": seq, "health": hp,
                                     "in_combat": in_combat, "options": options,
                                     "done_command_id": snap.get("done_command_id", 0),
                                     "pos": [pos.get("x", 0.0), pos.get("y", 0.0)]})
            echo(f"<< {bot} #{seq}: health {hp}/{hp_max}"
                 f"{', in combat' if in_combat else ''}; options: "
                 + "; ".join(f"{i}. {t_}" for i, t_ in enumerate(options, 1)))
            if not words or time.time() < next_at:
                continue
            for i, text in enumerate(options, 1):
                if text.lower().startswith(words[0].lower()):
                    sent += 1
                    cmd = {"bot": bot, "command_id": sent,
                           "choose": {"option": i, "seq": seq}}
                    send_frame(s, wire_py.encode_command(cmd))
                    out["commands"].append({"t": t, "command_id": sent, "option": i,
                                            "seq": seq, "text": text})
                    echo(f">> {bot} chooses {i} ({text}) of snapshot {seq}")
                    words.pop(0)
                    next_at = time.time() + 2
                    if not words and tail_s is not None:
                        stop_at = min(stop_at, time.time() + tail_s)
                    break
    s.close()
    out["never_offered"] = words
    if words:
        echo(f"never offered: {', '.join(words)}")
    return out


def whisper_script(seconds, sender, receiver, text, reply, echo=print):
    """Makes `sender` whisper `text` to `receiver` once both are online (a Say command), and
    `receiver` whisper `reply` back once it has heard it. Returns what each bot heard, the
    commands sent and any error events."""
    s = connect()
    reader = FrameReader()
    out = {"heard": [], "commands": [], "errors": [], "closed": False}
    seen = set()
    sent = 0
    replied = False
    start = time.time()
    stop_at = start + seconds

    def say(bot, to, words):
        nonlocal sent
        sent += 1
        cmd = {"bot": bot, "command_id": sent,
               "say": {"channel": "WHISPER", "to": to, "text": words}}
        send_frame(s, wire_py.encode_command(cmd))
        out["commands"].append({"t": round(time.time() - start, 3), **cmd})
        echo(f">> {bot} whispers to {to}: {words}")

    while time.time() < stop_at:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            out["closed"] = True
            break
        for payload in reader.feed(chunk):
            message = wire_py.decode_module_message(payload)
            if not message:
                continue
            t = round(time.time() - start, 3)
            if "snapshot" in message:
                bot = message["snapshot"].get("bot", "")
                if bot not in seen:
                    seen.add(bot)
                    echo(f"<< first snapshot of {bot}")
                    if {sender, receiver} <= seen and sent == 0:
                        say(sender, receiver, text)
                continue
            imm = message.get("event", {})
            event = imm.get("event", {})
            if "error" in event:
                out["errors"].append({"t": t, "bot": imm.get("bot", ""), **event["error"]})
                echo(f"<< error for {imm.get('bot', '')}: {event['error']}")
            chat = event.get("chat")
            if not chat:
                continue
            out["heard"].append({"t": t, "bot": imm.get("bot", ""), **chat})
            echo(f"<< {imm.get('bot', '')} heard {chat}")
            if imm.get("bot") == receiver and chat.get("from") == sender and not replied:
                replied = True
                say(receiver, sender, reply)
            elif imm.get("bot") == sender and chat.get("from") == receiver:
                stop_at = min(stop_at, time.time() + 1)
    s.close()
    return out


def run_walk(seconds, as_json=False):
    """Sends Go to / Explore choices to every bot that connects, cycling destinations every 15 s
    so all bots walk in ~70 yd mmap steps concurrently across the measurement window."""
    s = connect()
    reader = FrameReader()
    sent = 0
    snapshots = 0
    last_sent = {}
    choice_idx = collections.defaultdict(int)
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            break
        now = time.time()
        for payload in reader.feed(chunk):
            message = wire_py.decode_module_message(payload)
            if not message or "snapshot" not in message:
                continue
            snapshots += 1
            snap = message["snapshot"]
            bot = snap.get("bot", "")
            seq = snap.get("seq", 0)
            if now - last_sent.get(bot, 0.0) < 15.0:
                continue
            options = snap.get("options", [])
            walk_opts = [i for i, o in enumerate(options, 1)
                         if o.get("text", "").startswith(("Go to ", "Explore "))]
            if not walk_opts:
                continue
            pick = walk_opts[choice_idx[bot] % len(walk_opts)]
            choice_idx[bot] += 1
            sent += 1
            last_sent[bot] = now
            cmd = {"bot": bot, "command_id": sent,
                   "choose": {"option": pick, "seq": seq}}
            send_frame(s, wire_py.encode_command(cmd))
    s.close()
    out = {"bots_walked": len(last_sent), "commands_sent": sent, "snapshots": snapshots}
    if as_json:
        print(json.dumps(out))
    else:
        print(f"walked {out['bots_walked']} bots ({sent} walk commands, {snapshots} snapshots)")


def main():
    args = sys.argv[1:]
    stats = "--stats" in args
    walk = "--walk" in args
    choose = "--choose" in args
    whisper = "--whisper" in args
    as_json = "--json" in args
    tail = next((float(a.split("=", 1)[1]) for a in args if a.startswith("--tail=")), None)
    args = [a for a in args if a not in ("--stats", "--walk", "--choose", "--whisper", "--json")
            and not a.startswith("--tail=")]
    seconds = float(args[0]) if args else (10.0 if stats else 5.0)
    if stats:
        run_stats(seconds, as_json)
    elif walk:
        run_walk(seconds, as_json)
    elif whisper:
        echo = (lambda line: print(line, file=sys.stderr)) if as_json else print
        transcript = whisper_script(seconds, *args[1:5], echo=echo)
        if as_json:
            print(json.dumps(transcript))
    elif choose:
        if as_json:
            transcript = play_script(
                seconds, args[1:], echo=lambda line: print(line, file=sys.stderr), tail_s=tail)
            print(json.dumps(transcript))
        else:
            play_script(seconds, args[1:], tail_s=tail)
    else:
        run_print(seconds, args[1:])


if __name__ == "__main__":
    main()
