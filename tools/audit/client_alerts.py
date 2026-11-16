"""Which client events make the 3.3.5a default UI alert the player.

Reads the extracted UI code (tools/extract-client-ui.sh) and, for every event
handler branch (`event == "X"` up to the next branch or the end of the
function), records the alert actions in it: sounds, popups, flashes (including
the low-health screen flash), raid notices, error lines, alert frames, zone
text, combat feedback (damage on a portrait: how a player notices being hit,
since the 3.3.5a UI plays no aggro sound), and frames that appear by themselves. An alert is the game's own
answer to "is this worth a player's attention right now".

Alerts made through helper functions count too: a branch that calls a Lua
function which alerts (directly or through further calls) alerts.

Static and approximate (Lua is not parsed). Functions the client implements in
C are invisible here, so the `c_calls` column lists the functions each branch
calls that aren't defined in the Lua, for review (ShowReadyCheck, for one,
opens the ready-check popup in C).

Output: TSV event, alert kinds, files, c_calls.
Usage: python3 -I tools/audit/client_alerts.py [out_tsv]
"""

import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
UI = ROOT / "out" / "client-ui" / "Interface"

ALERTS = {
    "sound": re.compile(r"\bPlaySound(File)?\s*\("),
    "popup": re.compile(r"\bStaticPopup_Show\s*\("),
    "raid_notice": re.compile(r"\bRaidNotice_AddMessage\s*\("),
    "error_line": re.compile(r"\bUIErrorsFrame:AddMessage\b"),
    "flash": re.compile(r"\bUIFrameFlash\s*\(|\bFCF_StartAlertFlash\b|\bFlashClientIcon\b"
                        r"|\bLowHealthFrame_StartFlashing\b"),
    "alert_frame": re.compile(r"\w*Alert\w*_(Show|ShowAlert|AddAlert)\w*\s*\("
                              r"|AlertFrame\w*:Show\s*\("),
    "zone_text": re.compile(r"\bZoneText\w*|\bSubZoneText\w*"),
    "combat_feedback": re.compile(r"\bCombatFeedback_OnCombatEvent\s*\("),
    # A dialog or window appears by itself (the ready check is its own frame, not
    # a StaticPopup). Includes panels opened by the player's own action (a
    # vendor window on talking to a vendor): the audit tells them apart.
    "shows_frame": re.compile(r"\b\w+(Frame|Dialog|Popup)\w*:Show\s*\(\s*\)|\bShowUIPanel\s*\("),
}
BRANCH = re.compile(r'event\s*==\s*"([A-Z_0-9]+)"')
REGISTER = re.compile(r'RegisterEvent\s*\(\s*"([A-Z_0-9]+)"')
FUNCTION_END = re.compile(r"^end\b|^\s*</On\w+>|^\s*</Script>", re.M)
FUNCTION = re.compile(r"^function\s+([\w.:]+)\s*\(", re.M)
CALL = re.compile(r"\b([A-Za-z_]\w*(?:[.:][A-Za-z_]\w*)?)\s*\(")


def norm(name):
    return name.replace(":", ".")


def function_alerts(files):
    """Alert kinds per Lua function, following calls to a fixed point."""
    alerts, calls = {}, {}
    for text in files.values():
        for m in FUNCTION.finditer(text):
            stop = FUNCTION_END.search(text, m.end())
            body = text[m.end():stop.start() if stop else len(text)]
            name = norm(m.group(1))
            alerts[name] = {k for k, p in ALERTS.items() if p.search(body)}
            calls[name] = {norm(c) for c in CALL.findall(body)}
    changed = True
    while changed:
        changed = False
        for name, callees in calls.items():
            for callee in callees:
                extra = alerts.get(callee, set()) - alerts[name]
                if extra:
                    alerts[name] |= extra
                    changed = True
    return alerts


def body_alerts(body, functions):
    kinds = {k for k, p in ALERTS.items() if p.search(body)}
    for callee in CALL.findall(body):
        kinds |= functions.get(norm(callee), set())
    return kinds


def c_calls(body, functions):
    """Global functions called but not defined in the Lua: the client's C API."""
    return {c for c in CALL.findall(body)
            if "." not in c and ":" not in c and c[0].isupper() and c not in functions}


def scan(path, text, functions, found, api):
    rel = str(path.relative_to(UI))
    branches = list(BRANCH.finditer(text))
    for i, m in enumerate(branches):
        start = m.end()
        end = branches[i + 1].start() if i + 1 < len(branches) else len(text)
        stop = FUNCTION_END.search(text, start, end)
        body = text[start:stop.start() if stop else end]
        for kind in body_alerts(body, functions):
            found[m.group(1)][kind].add(rel)
        api[m.group(1)] |= c_calls(body, functions)
    # A frame that registers exactly one event and alerts in its OnEvent handler
    # without branching on `event` (common in small frames).
    registered = set(REGISTER.findall(text))
    if len(registered) == 1 and not branches:
        (event,) = registered
        for kind in body_alerts(text, functions):
            found[event][kind].add(rel + " (single-event frame)")


def main():
    files = {path: path.read_text(errors="replace") for path in sorted(UI.rglob("*"))
             if path.suffix.lower() in (".lua", ".xml")}
    functions = function_alerts(files)
    found = collections.defaultdict(lambda: collections.defaultdict(set))
    api = collections.defaultdict(set)
    for path, text in files.items():
        scan(path, text, functions, found, api)
    out = sys.argv[1] if len(sys.argv) > 1 else ROOT / "out" / "client-ui" / "alerts.tsv"
    with open(out, "w") as f:
        for event in sorted(set(found) | set(api)):
            kinds = found.get(event, {})
            files_ = sorted({p for ps in kinds.values() for p in ps})
            f.write(f"{event}\t{','.join(sorted(kinds))}\t{'; '.join(files_)}\t"
                    f"{','.join(sorted(api.get(event, ())))}\n")
    print(f"{len(found)} events with an alert -> {out}")


if __name__ == "__main__":
    main()
