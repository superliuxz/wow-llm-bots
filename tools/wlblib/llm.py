"""`wlb llm`: llama-server launcher and shared HTTP client helpers on 127.0.0.1:8090."""

import json
import os
import urllib.request

from wlblib.common import OUT, ROOT, need

DEFAULT_MODEL = "gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf"
DEFAULT_URL = "http://127.0.0.1:8090"


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


def health_check(base_url=DEFAULT_URL, timeout=2):
    try:
        with urllib.request.urlopen(f"{base_url.rstrip('/')}/health", timeout=timeout) as r:
            return r.status == 200
    except OSError:
        return False


def loaded_model_matches(model, base_url=DEFAULT_URL, timeout=2):
    if not model:
        return True
    try:
        with urllib.request.urlopen(f"{base_url.rstrip('/')}/v1/models", timeout=timeout) as r:
            return model in r.read().decode(errors="replace")
    except OSError:
        return False


def chat_completion(messages, *, base_url=DEFAULT_URL, temperature=0.0,
                    max_tokens=150, grammar=None, timeout=60):
    """Posts one `/v1/chat/completions` request and returns the assistant text."""
    body = {"messages": messages, "temperature": temperature, "max_tokens": max_tokens}
    if grammar:
        body["grammar"] = grammar
    req = urllib.request.Request(
        f"{base_url.rstrip('/')}/v1/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"]
