"""Client for the nfsu2_probe TCP server (JSON lines on 127.0.0.1). Used by mcp_bridge.py and probe.py."""
from __future__ import annotations

import base64
import io
import itertools
import json
import os
import socket
import time
from pathlib import Path

DEFAULT_PORT = int(os.environ.get("NFSU2_PROBE_PORT", "47800"))
ROOT = Path(__file__).resolve().parents[2]
SHOT_DIR = ROOT / "work" / "screenshots"

_ids = itertools.count(1)


class ProbeError(RuntimeError):
    pass


def call(method: str, params: dict | None = None, *, port: int = DEFAULT_PORT, timeout: float = 15.0):
    """One request per connection: robust across game restarts."""
    req = {"id": next(_ids), "method": method, "params": params or {}}
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.sendall((json.dumps(req) + "\n").encode())
            buf = bytearray()
            while not buf.endswith(b"\n"):
                chunk = s.recv(1 << 20)
                if not chunk:
                    break
                buf += chunk
    except (ConnectionRefusedError, socket.timeout, OSError) as e:
        raise ProbeError(f"probe not reachable on 127.0.0.1:{port}: {e}") from e
    if not buf:
        raise ProbeError("empty response from probe")
    resp = json.loads(buf)
    if "error" in resp:
        raise ProbeError(resp["error"])
    return resp["result"]


def wait_ready(timeout: float = 60.0, port: int = DEFAULT_PORT) -> dict:
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            return call("ping", port=port, timeout=2.0)
        except ProbeError as e:
            last = e
            time.sleep(0.5)
    raise ProbeError(f"probe not ready after {timeout}s: {last}")


def wait_frames(timeout: float = 60.0, min_frames: int = 2, port: int = DEFAULT_PORT) -> dict:
    """Wait until the probe answers and the game has presented at least `min_frames` frames."""
    end = time.monotonic() + timeout
    r = wait_ready(timeout, port)
    while time.monotonic() < end:
        if r.get("frames", 0) >= min_frames:
            return r
        time.sleep(0.25)
        try:
            r = call("ping", port=port, timeout=2.0)
        except ProbeError:
            pass
    raise ProbeError(f"no frames after {timeout}s (last ping: {r})")


KEEP_SHOTS = 20


def prune_shots(keep: int = KEEP_SHOTS) -> None:
    """Keep only the newest `keep` screenshots in work/screenshots (references live in work/oracle, work/boot_refs)."""
    shots = sorted(SHOT_DIR.glob("shot_*.png"), key=lambda p: p.stat().st_mtime)
    for p in shots[:-keep]:
        p.unlink(missing_ok=True)


def screenshot_png(scale: int = 2, port: int = DEFAULT_PORT, save: bool = True) -> tuple[bytes, dict, Path | None]:
    """Returns (png bytes, metadata, saved path)."""
    from PIL import Image

    r = call("screenshot", {"scale": scale}, port=port, timeout=30.0)
    raw = base64.b64decode(r.pop("data"))
    im = Image.frombuffer("RGB", (r["width"], r["height"]), raw, "raw", "BGRX", 0, 1)
    out = io.BytesIO()
    im.save(out, format="PNG", optimize=False)
    png = out.getvalue()
    path = None
    if save:
        SHOT_DIR.mkdir(parents=True, exist_ok=True)
        path = SHOT_DIR / time.strftime(f"shot_%Y%m%d_%H%M%S_f{r['frame']}.png")
        path.write_bytes(png)
        prune_shots()
    return png, r, path


def reload(timeout: float = 15.0, port: int = DEFAULT_PORT) -> dict:
    """Ask the loader to reload the core now and wait until the new core answers."""
    before = call("ping", port=port)["reload_count"]
    call("reload", port=port)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        time.sleep(0.2)
        try:
            r = call("ping", port=port, timeout=2.0)
            if r["reload_count"] > before:
                return r
        except ProbeError:
            pass  # server is restarting
    raise ProbeError("core did not come back after reload")


def key(name, action: str = "press", ms: int = 100, port: int = DEFAULT_PORT) -> dict:
    """Inject a DirectInput key. name: DIK name ("enter", "up", "a", ...) or code. action: press|hold|release."""
    import dik

    return call("key", {"key": dik.code(name), "action": action, "ms": ms}, port=port)


def hexdump(addr: int, data: bytes, width: int = 16) -> str:
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off : off + width]
        hx = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{addr + off:08x}  {hx:<{width * 3}} {asc}")
    return "\n".join(lines)
