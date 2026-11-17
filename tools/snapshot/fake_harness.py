"""Minimal stand-in for the harness, for testing mod-llm-brain's socket.

Connects to the brain socket and, for a few seconds, either prints what the server sends or
measures how it arrives. Messages are length-prefixed frames (4-byte big-endian length, then
the payload), as in common/net/frame_codec.h. Module messages (wlb.snapshot.ModuleMessage) are
printed as JSON when the generated Python code is available.

Dev tool only (production is C++). For JSON, generate the code once with
tools/wlb check snapshots and run with the tools venv:
  out/tools-venv/bin/python -I tools/snapshot/fake_harness.py [seconds=5] [message ...]
  out/tools-venv/bin/python -I tools/snapshot/fake_harness.py --stats [seconds=10]
  out/tools-venv/bin/python -I tools/snapshot/fake_harness.py --choose [--json] [--tail=S] [seconds=30] Attack Loot

--stats prints nothing per frame. It reports, from the snapshots' own timestamps (monotonic_us:
this machine's monotonic clock, the same in every process): frames per second; the age of a snapshot when it arrives (built to
received: world thread, socket, this process); how evenly each bot's snapshots are spaced; and
how bunched the arrivals are (the most frames arriving within 10 ms), which shows whether the
module builds all bots on the same tick.

--choose plays a scripted brain (play_script): for each word given, in turn, it waits for a
snapshot with an option starting with that word (case-insensitive), sends a Command.Choose for
it (that option of that snapshot), and then waits at least 2 s before looking for the next
word. It prints one line per snapshot (seq, health, in combat, the options) and every immediate
event (a failed choice comes back as an error event). The module's log has the matching
"chose ... age_at_apply" line. With --json it prints nothing else: the whole transcript as one
JSON object (what `tools/wlb test e2e` reads).
"""

import collections
import json
import pathlib
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from frame_reader import FrameReader  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SOCK = str(ROOT / "out" / "server" / "run" / "brain.sock")

try:
    sys.path.insert(0, str(ROOT / "out" / "proto"))
    from google.protobuf import json_format
    from proto import snapshot_pb2
except ImportError:
    snapshot_pb2 = None


def describe(payload):
    if snapshot_pb2 is not None:
        message = snapshot_pb2.ModuleMessage()
        try:
            message.ParseFromString(payload)
            return json_format.MessageToJson(message, indent=1, preserving_proto_field_name=True)
        except Exception:  # Not a ModuleMessage: fall through to hex.
            pass
    return f"[{len(payload)} bytes] {payload[:32].hex()}"


def percentile(sorted_values, q):
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))] if sorted_values else 0


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.2)
    return s


def run_stats(seconds, warmup=2.0):
    s = connect()
    reader = FrameReader()
    ages, arrivals = [], []  # age in ms; arrival time in s.
    per_bot = collections.defaultdict(list)  # bot -> built-at times (ms).
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            print("server closed the connection")
            break
        # The snapshot's monotonic_us is CLOCK_MONOTONIC, which time.monotonic_ns() reads
        # on Linux: a clock that the wall clock being set can't move.
        received_ms = time.monotonic_ns() / 1e6
        for payload in reader.feed(chunk):
            message = snapshot_pb2.ModuleMessage()
            message.ParseFromString(payload)
            if not message.HasField("snapshot"):
                continue
            snap = message.snapshot
            built_ms = snap.monotonic_us / 1000
            ages.append(received_ms - built_ms)
            arrivals.append(received_ms / 1000)
            per_bot[snap.bot].append(built_ms)
    s.close()
    if not ages:
        print("no snapshots received")
        return
    def bunching(times):
        """The most frames arriving within any 10 ms window."""
        window, most = collections.deque(), 0
        for t in times:
            window.append(t)
            while t - window[0] > 0.010:
                window.popleft()
            most = max(most, len(window))
        return most

    first = arrivals[0]
    # The first round is every bot's first snapshot, all overdue when the harness connects (a
    # burst by design): reported on its own, and left out of the steady-state numbers.
    steady = [i for i, t in enumerate(arrivals) if t - first >= warmup]
    print(f"{len(ages)} snapshots from {len(per_bot)} bots in {seconds:.0f}s "
          f"({len(ages) / seconds:.1f}/s)")
    print(f"first {warmup:.0f} s (every bot's first snapshot): up to "
          f"{bunching([t for t in arrivals if t - first < warmup])} within 10 ms")
    if not steady:
        print("(no steady state: run longer)")
        return
    steady_ages = sorted(ages[i] for i in steady)
    steady_from_ms = arrivals[steady[0]] * 1000
    gaps = sorted(b - a for times in per_bot.values()
                  for a, b in zip(times, times[1:]) if a >= steady_from_ms - 1500)
    print(f"steady state ({len(steady)} snapshots):")
    print(f"  age on arrival (built -> received): p50 {percentile(steady_ages, .5):.1f} ms, "
          f"p95 {percentile(steady_ages, .95):.1f} ms, max {steady_ages[-1]:.1f} ms")
    if gaps:
        print(f"  spacing of one bot's snapshots: p5 {percentile(gaps, .05):.0f} ms, "
              f"p50 {percentile(gaps, .5):.0f} ms, p95 {percentile(gaps, .95):.0f} ms")
    print(f"  bunching: up to {bunching([arrivals[i] for i in steady])} snapshots "
          f"arrived within 10 ms")


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
    """A scripted brain: for each word in `words`, in turn, waits for a snapshot with an option
    starting with it (case-insensitive) and sends a Command.Choose for that option of that
    snapshot, then waits at least 2 s before the next word. Returns what happened:
      {"snapshots": [{"t", "seq", "health", "in_combat", "options": [text...]}...],
       "events": [{"t", "event": {...}}...],          # immediate events (errors, ...)
       "commands": [{"t", "command_id", "option", "seq", "text"}...],
       "never_offered": [word...],                    # words never found in time
       "closed": bool}                                # the server closed the connection
    `t` is seconds since the start. `echo` gets one line per snapshot, event and command. With
    `tail_s`, it stops that long after the last word was sent (instead of at `seconds`)."""
    s = connect()
    reader = FrameReader()
    words = list(words)
    out = {"snapshots": [], "events": [], "commands": [], "never_offered": [], "closed": False}
    start = time.time()
    next_at = 0.0
    sent = 0  # Also each command's id, so a failure names the command it belongs to.
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
            message = snapshot_pb2.ModuleMessage()
            message.ParseFromString(payload)
            t = round(time.time() - start, 3)
            if message.HasField("event"):
                event = json_format.MessageToDict(message.event.event,
                                                  preserving_proto_field_name=True)
                out["events"].append({"t": t, "event": event})
                echo(f"<< event for {message.event.bot}: {event}")
                continue
            snap = message.snapshot
            me = snap.base.self
            options = [o.text for o in snap.options]
            out["snapshots"].append({"t": t, "seq": snap.seq, "health": me.health,
                                     "in_combat": me.in_combat, "options": options})
            echo(f"<< {snap.bot} #{snap.seq}: health {me.health}/{me.health_max}"
                 f"{', in combat' if me.in_combat else ''}; options: "
                 + "; ".join(f"{i}. {t_}" for i, t_ in enumerate(options, 1)))
            if not words or time.time() < next_at:
                continue
            for i, text in enumerate(options, 1):
                if text.lower().startswith(words[0].lower()):
                    sent += 1
                    command = snapshot_pb2.Command(bot=snap.bot, command_id=sent)
                    command.choose.option = i
                    command.choose.seq = snap.seq
                    send_frame(s, command.SerializeToString())
                    out["commands"].append({"t": t, "command_id": sent, "option": i,
                                            "seq": snap.seq, "text": text})
                    echo(f">> {snap.bot} chooses {i} ({text}) of snapshot {snap.seq}")
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


def main():
    args = sys.argv[1:]
    stats = "--stats" in args
    choose = "--choose" in args
    as_json = "--json" in args
    tail = next((float(a.split("=", 1)[1]) for a in args if a.startswith("--tail=")), None)
    args = [a for a in args if a not in ("--stats", "--choose", "--json")
            and not a.startswith("--tail=")]
    seconds = float(args[0]) if args else (10.0 if stats else 5.0)
    if stats:
        run_stats(seconds)
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
