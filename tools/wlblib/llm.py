"""`wlb llm`: llama-server with the brain's model on 127.0.0.1:8090, in the foreground.

The defaults are the fastest settings measured for 50-200 bots (docs/phase2-results.md): 16 slots,
4k context each, bigger batches (-ub 2048 -b 4096).
"""

import os

from wlblib.common import OUT, ROOT, need

# The more accurate model within the speed limit (docs/TODO.md, "Model choice"): Gemma 4 scored
# 44/48 on the eval against Qwen3.6's 35/48, 16 ms slower to the first line. `MODEL=` picks another.
DEFAULT_MODEL = "gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf"


def serve(slots=16, ctx_per_slot=4096, model=None, extra=()):
    model = model or os.environ.get("MODEL", DEFAULT_MODEL)
    server = OUT / "llama" / "bin" / "llama-server"
    need(server, "tools/wlb setup llama")
    need(ROOT / "models" / model, "download the model into models/")
    args = [str(server), "-m", str(ROOT / "models" / model), "--host", "127.0.0.1",
            "--port", "8090", "-ngl", "99", "-fa", "on", "-np", str(slots),
            "-c", str(slots * ctx_per_slot), "-ctk", "q8_0", "-ctv", "q8_0",
            "-ub", "2048", "-b", "4096", "--jinja", "--no-webui", *extra]
    os.execv(args[0], args)  # Replaces this process: Ctrl-C stops the server.
