"""BEVE wire wrapper, length-prefixed frame reader, hover check, and `wlb snap` tools.

Consolidates the former `frame_reader.py`, `wire_py.py`, `check_hover_fields.py`,
`capture_snapshots.py`, and `fake_harness.py` helpers into one module.
"""

from __future__ import annotations

import collections
import ctypes
import json
import pathlib
import re
import socket
import sys
import time

from wlblib.common import OUT, ROOT

SO_PATH = OUT / "common" / "libwlb_wire_py.so"
DEFAULT_SOCK = str(ROOT / "out" / "server" / "run" / "brain.sock")

_lib = None


class FrameReader:
    """Splits a stream of 4-byte big-endian length-prefixed frames."""

    HEADER = 4

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        self._buf += data
        frames: list[bytes] = []
        buf = self._buf
        pos = 0
        while len(buf) - pos >= self.HEADER:
            size = int.from_bytes(buf[pos:pos + self.HEADER], "big")
            end = pos + self.HEADER + size
            if len(buf) < end:
                break
            frames.append(bytes(buf[pos + self.HEADER:end]))
            pos = end
        if pos:
            del buf[:pos]
        return frames


def _load():
    global _lib
    if _lib is not None:
        return _lib
    if not SO_PATH.exists():
        return None
    lib = ctypes.CDLL(str(SO_PATH))
    lib.wlb_module_message_beve_to_json.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_module_message_beve_to_json.restype = ctypes.c_void_p
    lib.wlb_command_beve_to_json.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_command_beve_to_json.restype = ctypes.c_void_p
    lib.wlb_command_json_to_beve.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_command_json_to_beve.restype = ctypes.c_void_p
    lib.wlb_module_message_visibility_errors.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_module_message_visibility_errors.restype = ctypes.c_void_p
    lib.wlb_hello_command_json.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_hello_command_json.restype = ctypes.c_void_p
    lib.wlb_wire_free.argtypes = [ctypes.c_void_p]
    lib.wlb_wire_free.restype = None
    _lib = lib
    return lib


def module_message_to_json(payload: bytes, pretty: bool = False):
    lib = _load()
    if lib is None:
        return None
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_module_message_beve_to_json(
        payload, len(payload), 1 if pretty else 0, ctypes.byref(out_len)
    )
    if not ptr:
        return None
    try:
        return ctypes.string_at(ptr, out_len.value).decode("utf-8")
    finally:
        lib.wlb_wire_free(ptr)


class WireMismatch(RuntimeError):
    """The module's hello names another build than `libwlb_wire_py.so`'s."""


def hello_command(role: str) -> dict:
    """This build's hello as a Command dict ({"version", "hello": {wire_version, schema_hash,
    role}}): what a tool may send first (`send_frame(sock, encode_command(...))`)."""
    lib = _load()
    if lib is None:
        raise RuntimeError(f"missing {SO_PATH}: run `tools/wlb build common --no-tests`")
    raw = role.encode("utf-8")
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_hello_command_json(raw, len(raw), ctypes.byref(out_len))
    try:
        return json.loads(ctypes.string_at(ptr, out_len.value).decode("utf-8"))
    finally:
        lib.wlb_wire_free(ptr)


def hello_mismatch(hello: dict) -> str:
    """Why the module's `hello` can't talk to this build ("" when it can)."""
    mine = hello_command("")["hello"]
    if hello.get("wire_version", 0) != mine["wire_version"]:
        return (f"module speaks wire version {hello.get('wire_version', 0)}, these tools "
                f"{mine['wire_version']}: rebuild (`tools/wlb build common` / `build server`)")
    if hello.get("schema_hash", "") != mine["schema_hash"]:
        return (f"module has wire schema {hello.get('schema_hash', '')}, these tools "
                f"{mine['schema_hash']}: rebuild (`tools/wlb build common` / `build server`)")
    return ""


def decode_module_message(payload: bytes):
    """A module frame as a dict, or None. The module's hello (the first frame of every
    connection) is checked and consumed here, so readers only ever see snapshots and events;
    one of another build raises WireMismatch."""
    text = module_message_to_json(payload, pretty=False)
    if text is None:
        return None
    message = json.loads(text)
    if "hello" in message:
        mismatch = hello_mismatch(message["hello"])
        if mismatch:
            raise WireMismatch(mismatch)
        return None
    return message


def decode_command(payload: bytes):
    lib = _load()
    if lib is None:
        return None
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_command_beve_to_json(payload, len(payload), ctypes.byref(out_len))
    if not ptr:
        return None
    try:
        return json.loads(ctypes.string_at(ptr, out_len.value).decode("utf-8"))
    finally:
        lib.wlb_wire_free(ptr)


def encode_command(command_dict: dict) -> bytes:
    lib = _load()
    if lib is None:
        raise RuntimeError(f"missing {SO_PATH}: run `tools/wlb build common --no-tests`")
    raw = json.dumps(command_dict).encode("utf-8")
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_command_json_to_beve(raw, len(raw), ctypes.byref(out_len))
    if not ptr:
        raise ValueError(f"invalid Command dict: {command_dict!r}")
    try:
        return ctypes.string_at(ptr, out_len.value)
    finally:
        lib.wlb_wire_free(ptr)


def visibility_errors(payload: bytes):
    lib = _load()
    if lib is None:
        return None
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_module_message_visibility_errors(
        payload, len(payload), ctypes.byref(out_len)
    )
    if not ptr:
        return None
    try:
        text = ctypes.string_at(ptr, out_len.value).decode("utf-8")
    finally:
        lib.wlb_wire_free(ptr)
    return [line for line in text.split("\n") if line]


def send_frame(sock: socket.socket, payload: bytes) -> None:
    sock.sendall(len(payload).to_bytes(4, "big") + payload)


def check_hover_fields() -> int:
    """Checks `kHoverFields` in snapshot_wire.cc against `visibility: HOVER` tags in snapshot.h."""
    header = (ROOT / "snapshot/snapshot.h").read_text()
    want, struct, previous = [], None, ""
    for line in header.splitlines():
        m = re.match(r"^struct (\w+) \{", line)
        if m:
            struct = m.group(1)
        code = line.split("//")[0]
        if "visibility: HOVER" in line:
            own = re.search(r"\w+\s*(=[^;]*)?;", code) and not re.match(r"\s*\w+::", code)
            names = re.findall(r"(\w+)\s*(?:=[^;]*)?;", code if own else previous + " " + code)
            if not names:
                raise SystemExit(f"can't read the field on: {line.strip()}")
            want.append(f"{struct}.{names[-1]}")
        previous = code
    source = (ROOT / "snapshot/snapshot_wire.cc").read_text()
    m = re.search(r"kHoverFields\[\] = \{(.*?)\};", source, re.S)
    if not m:
        raise SystemExit("no kHoverFields in snapshot/snapshot_wire.cc")
    have = re.findall(r'"([\w.]+)"', m.group(1))
    missing = sorted(set(want) - set(have))
    extra = sorted(set(have) - set(want))
    if missing or extra:
        print("kHoverFields (snapshot/snapshot_wire.cc) differs from snapshot/snapshot.h:",
              file=sys.stderr)
        for name in missing:
            print(f"  tagged, not listed: {name}", file=sys.stderr)
        for name in extra:
            print(f"  listed, not tagged: {name}", file=sys.stderr)
        return 1
    print(f"hover fields: {len(want)} tagged, all checked")
    return 0


def capture_snapshots(out_dir: pathlib.Path, seconds: float = 15.0,
                      sock_path: str = DEFAULT_SOCK) -> int:
    """Saves live snapshots from mod-llm-brain's socket as `<out_dir>/<bot>/<seq>.json`."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(0.2)
    reader = FrameReader()
    frames: list[bytes] = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            print("server closed the connection")
            break
        frames += reader.feed(chunk)
    s.close()
    saved = 0
    seen: set[str] = set()
    for payload in frames:
        message = decode_module_message(payload)
        if not message or "snapshot" not in message:
            continue
        snap = message["snapshot"]
        bot = snap.get("bot", "unknown")
        seq = snap.get("seq", 0)
        path = out_dir / bot / f"{seq}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(snap))
        saved += 1
        seen.add(bot)
    print(f"saved {saved} snapshots from {len(seen)} bots in {out_dir}")
    return 0 if saved > 0 else 1


def _connect(sock_path: str, role: str | None = None) -> socket.socket:
    """Connects to the module; with a `role`, says hello first (passive readers needn't)."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(0.2)
    if role is not None:
        send_frame(s, encode_command(hello_command(role)))
    return s


def _note_visibility(payload: bytes, out: dict) -> None:
    errors = visibility_errors(payload)
    if errors is None:
        if len(out["visibility_errors"]) < 20:
            out["visibility_errors"].append(
                "failed to decode module frame (libwlb_wire_py.so missing or wire mismatch)"
            )
        return
    if errors and len(out["visibility_errors"]) < 20:
        out["visibility_errors"].extend(errors[:5])


def _percentile(sorted_values: list[float], q: float) -> float:
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))] if sorted_values else 0.0


def measure_delivery(sock_path: str, seconds: float, warmup: float = 2.0) -> dict:
    s = _connect(sock_path)
    reader = FrameReader()
    ages, arrivals = [], []
    per_bot: dict[str, list[float]] = collections.defaultdict(list)
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
            message = decode_module_message(payload)
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
    out: dict = {
        "snapshots": len(ages), "bots": len(per_bot), "per_s": len(ages) / seconds,
        "warmup_s": warmup,
        "first_bunching": bunching([t for t in arrivals if t - first < warmup]),
    }
    steady = [i for i, t in enumerate(arrivals) if t - first >= warmup]
    if steady:
        steady_ages = sorted(ages[i] for i in steady)
        steady_from_ms = arrivals[steady[0]] * 1000
        gaps = sorted(
            b - a for times in per_bot.values()
            for a, b in zip(times, times[1:]) if a >= steady_from_ms - 1500
        )
        out["steady"] = {
            "n": len(steady),
            "age_p50_ms": _percentile(steady_ages, 0.5),
            "age_p95_ms": _percentile(steady_ages, 0.95),
            "age_max_ms": steady_ages[-1],
            "spacing_p5_ms": _percentile(gaps, 0.05),
            "spacing_p50_ms": _percentile(gaps, 0.5),
            "spacing_p95_ms": _percentile(gaps, 0.95),
            "bunching": bunching([arrivals[i] for i in steady]),
        }
    return out


def play_script(sock_path: str, seconds: float, words, echo=print, tail_s=None) -> dict:
    s = _connect(sock_path, role="wlb snap fake")
    reader = FrameReader()
    words = list(words)
    out = {"snapshots": [], "events": [], "commands": [], "never_offered": [], "closed": False,
           "visibility_errors": []}
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
            _note_visibility(payload, out)
            message = decode_module_message(payload)
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
            out["snapshots"].append({
                "t": t, "seq": seq, "health": hp, "health_max": hp_max,
                "power": me.get("power", 0), "power_max": me.get("power_max", 0),
                "in_combat": in_combat, "options": options,
                "done_command_id": snap.get("done_command_id", 0),
                "open_panels": snap.get("base", {}).get("ui", {}).get("open_panels", []),
                "bag_items": [i.get("name", "") for i in
                              snap.get("ext", {}).get("bags", {}).get("items", [])],
                "tooltips": [t_.get("tooltip", {}).get("lines", [])
                             for t_ in snap.get("base", {}).get("tooltips", [])],
                "auras": [a.get("name", "") for a in me.get("unit", {}).get("auras", [])],
                "spells": [{"id": sp.get("id", 0), "name": sp.get("name", ""),
                            "rank": sp.get("rank", 0),
                            "cooldown_left_s": sp.get("ability", {}).get(
                                "cooldown_remaining_s", 0.0)}
                           for sp in (snap.get("ext", {}).get("action_bars") or {}).get(
                               "buttons", [])],
                "pos": [pos.get("x", 0.0), pos.get("y", 0.0)],
            })
            echo(f"<< {bot} #{seq}: health {hp}/{hp_max}"
                 f"{', in combat' if in_combat else ''}; options: "
                 + "; ".join(f"{i}. {t_}" for i, t_ in enumerate(options, 1)))
            if not words or time.time() < next_at:
                continue
            verb, _, arg = words[0].partition(":")
            if verb in ("open", "close", "hover"):
                cmd: dict = {"bot": bot, "command_id": sent + 1}
                if verb == "hover":
                    bag = snap.get("ext", {}).get("bags", {}).get("items", [])
                    item = next((i for i in bag if i.get("name") == arg), None)
                    if not item:
                        continue
                    cmd["hover"] = {"ref": {"kind": "ITEM", "id": str(item["id"]), "name": arg}}
                else:
                    cmd[verb] = {"panel": arg.upper().replace(" ", "_")}
                sent += 1
                send_frame(s, encode_command(cmd))
                out["commands"].append({"t": t, "command_id": sent, "seq": seq, "text": words[0]})
                echo(f">> {bot} {words[0]}")
                words.pop(0)
                next_at = time.time() + 2
                if not words and tail_s is not None:
                    stop_at = min(stop_at, time.time() + tail_s)
                continue
            for i, text in enumerate(options, 1):
                if text.lower().startswith(words[0].lower()):
                    sent += 1
                    cmd = {"bot": bot, "command_id": sent, "choose": {"option": i, "seq": seq}}
                    send_frame(s, encode_command(cmd))
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


def whisper_script(sock_path: str, seconds: float, sender: str, receiver: str,
                   text: str, reply: str, echo=print) -> dict:
    s = _connect(sock_path)
    reader = FrameReader()
    out = {"heard": [], "commands": [], "errors": [], "closed": False, "visibility_errors": []}
    seen: set[str] = set()
    sent = 0
    replied = False
    start = time.time()
    stop_at = start + seconds

    def say(bot, to, words):
        nonlocal sent
        sent += 1
        cmd = {"bot": bot, "command_id": sent,
               "say": {"channel": "WHISPER", "to": to, "text": words}}
        send_frame(s, encode_command(cmd))
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
            _note_visibility(payload, out)
            message = decode_module_message(payload)
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


def run_walk(sock_path: str, seconds: float) -> dict:
    s = _connect(sock_path)
    reader = FrameReader()
    sent = 0
    snapshots = 0
    last_sent: dict[str, float] = {}
    choice_idx: dict[str, int] = collections.defaultdict(int)
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
            message = decode_module_message(payload)
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
            cmd = {"bot": bot, "command_id": sent, "choose": {"option": pick, "seq": seq}}
            send_frame(s, encode_command(cmd))
    s.close()
    return {"bots_walked": len(last_sent), "commands_sent": sent, "snapshots": snapshots}


def run_print(sock_path: str, seconds: float, to_send: list[str]) -> None:
    s = _connect(sock_path)
    for message in to_send:
        payload = message.encode()
        send_frame(s, payload)
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
            pretty = module_message_to_json(payload, pretty=True)
            print(f"<< {pretty if pretty is not None else f'[{len(payload)} B] {payload[:32].hex()}'}")
    s.close()
    print(f"received {received} messages in {seconds:.0f}s")


def fake_harness_cli(argv: list[str]) -> int:
    """CLI for `wlb snap fake` (replaces the retired fake_harness.py)."""
    sock_path = next((a.split("=", 1)[1] for a in argv if a.startswith("--socket=")), DEFAULT_SOCK)
    args = [a for a in argv if not a.startswith("--socket=")]
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
        r = measure_delivery(sock_path, seconds)
        print(json.dumps(r) if as_json else r)
    elif walk:
        r = run_walk(sock_path, seconds)
        print(json.dumps(r) if as_json else
              f"walked {r['bots_walked']} bots ({r['commands_sent']} cmds, {r['snapshots']} snaps)")
    elif whisper:
        echo = (lambda line: print(line, file=sys.stderr)) if as_json else print
        tr = whisper_script(sock_path, seconds, *args[1:5], echo=echo)
        if as_json:
            print(json.dumps(tr))
    elif choose:
        echo = (lambda line: print(line, file=sys.stderr)) if as_json else print
        tr = play_script(sock_path, seconds, args[1:], echo=echo, tail_s=tail)
        if as_json:
            print(json.dumps(tr))
    else:
        run_print(sock_path, seconds, args[1:])
    return 0
