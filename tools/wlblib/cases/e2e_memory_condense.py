"""Memory condensing with the real model (F5), through the real harness.

Loada starts with 30 `remember:` notes in its memory.md (over the 24-note threshold). After a
normal out-of-combat decision the harness asks the model (llama-server, the default brain) to
condense the older notes. memory.md must then hold an `## Earlier summary` of a few bullets that
keeps the names and places that matter, followed by the newest notes.
"""

import pathlib
import subprocess
import tempfile
import time

from wlblib import build
from wlblib.common import OUT, ROOT
from wlblib.testing import LlamaServer, expect

NAME = "memory_condense"
DESCRIPTION = "Real model condenses 30 memory notes into an Earlier summary"
TEST_CONFIG = "whisper"  # Two level-1 priests standing still.
BOT = "Loada"
ONLINE = 2

NOTES = (
    ["- 2026-11-01 Hele tanked Ragefire Chasm for us, good tank, patient with pulls",
     "- 2026-11-01 Taragaman the Hungerer hits hard, Hele kited him away from the healer",
     "- 2026-11-02 Kelra is a mage who keeps pulling extra packs in Wailing Caverns",
     "- 2026-11-02 learned to drink between pulls, healer mana ran out twice",
     "- 2026-11-03 want to finish Deadmines with Hele, we wiped twice on Mr. Smite",
     "- 2026-11-03 Hele said he plays most evenings, invite him for Deadmines"]
    + [f"- 2026-11-{4 + i // 4:02d} killed boars and scorpids near Razor Hill, nothing new ({i})"
       for i in range(24)]
)
KEY_WORDS = ("Hele", "Deadmines", "Kelra", "Ragefire", "Smite", "Wailing")


def run(stack):
    build.common("default", tests=False)
    with tempfile.TemporaryDirectory(prefix="wlb-e2e-memory-") as tmp, \
            LlamaServer(slots=4):
        bots_dir = pathlib.Path(tmp) / "bots"
        memory = bots_dir / "Loada" / "memory.md"
        memory.parent.mkdir(parents=True)
        memory.write_text("\n".join(NOTES) + "\n")
        harness = subprocess.Popen(
            [str(OUT / "common" / "harness"), "run", f"--socket={stack.socket}",
             f"--rules={ROOT / 'content' / 'RULES.md'}", f"--bots-dir={bots_dir}",
             "--slots=4", "--temperature=0"],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.time() + 180.0
            text = ""
            while time.time() < deadline:
                text = memory.read_text()
                if "## Earlier summary" in text:
                    break
                time.sleep(2.0)
        finally:
            harness.terminate()
            try:
                out, _ = harness.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                harness.kill()
                out, _ = harness.communicate()

    expect("## Earlier summary" in text,
           f"memory.md was not condensed within 180 s; harness log tail:\n{out[-1500:]}")
    summary_part, _, recent_part = text.partition("## Recent")
    print(summary_part.strip())
    summary = [ln for ln in summary_part.splitlines() if ln.startswith("- ")]
    recent = [ln for ln in recent_part.splitlines() if ln.startswith("- ")]
    kept = [w for w in KEY_WORDS if w in summary_part]
    expect(1 <= len(summary) <= 8, f"summary has {len(summary)} bullets:\n{summary_part}")
    expect(len(kept) >= 3, f"summary keeps only {kept} of {KEY_WORDS}:\n{summary_part}")
    expect(len(recent) < len(NOTES), f"recent notes not trimmed ({len(recent)} left)")
    expect("(23)" in recent_part, "the newest note did not stay under ## Recent")
    return (f"{len(NOTES)} notes -> {len(summary)}-bullet summary keeping {kept} "
            f"+ {len(recent)} recent")
