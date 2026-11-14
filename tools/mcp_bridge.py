#!/usr/bin/env python3
"""MCP stdio server: exposes the in-game probe to Claude Code.

Uses the official `mcp` SDK 2.3.0 (`mcp.server.mcpserver.MCPServer`). Each tool forwards to the probe's TCP
server (tools/probe_client.py). Addresses are accepted as ints or strings like "0x400000".
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp.server.mcpserver import Image, MCPServer  # noqa: E402

import probe_client as pc  # noqa: E402

server = MCPServer(
    name="nfsu2",
    instructions=(
        "Live access to Need for Speed Underground 2 (NA v1.2) running under Wine with our probe ASI. "
        "Start the game with tools/run_game.sh first. Addresses are for SPEED2.EXE v1.2 (image base 0x400000)."
    ),
)


def _addr(a: int | str) -> int:
    return a if isinstance(a, int) else int(a, 0)


@server.tool()
def ping() -> dict:
    """Check the probe is alive. Returns version, pid, uptime and rendered frame count."""
    return pc.call("ping")


@server.tool()
def info() -> dict:
    """Exe base/size/hash check, D3D device and window, frame count, thread ids."""
    return pc.call("info")


@server.tool()
def read_mem(addr: str, size: int = 64) -> str:
    """Read `size` bytes (max 1 MiB) at `addr` (e.g. "0x400000"). Returns a hex dump."""
    a = _addr(addr)
    r = pc.call("read_mem", {"addr": a, "size": size})
    return pc.hexdump(a, bytes.fromhex(r["hex"]))


@server.tool()
def write_mem(addr: str, hex: str) -> dict:
    """Write bytes (hex string, spaces allowed) at `addr`. Page protection is handled; icache is flushed."""
    return pc.call("write_mem", {"addr": _addr(addr), "hex": hex})


@server.tool()
def aob_scan(pattern: str, start: str | None = None, end: str | None = None, max: int = 64) -> dict:
    """Find a byte pattern like "8B 0D ?? ?? ?? ??" ("??" = any byte). Default range: the exe image."""
    p: dict = {"pattern": pattern, "max": max}
    if start:
        p["start"] = _addr(start)
    if end:
        p["end"] = _addr(end)
    return pc.call("aob_scan", p)


@server.tool()
def log_tail(n: int = 50) -> str:
    """Last `n` lines of the probe log."""
    return "\n".join(l["text"] for l in pc.call("log_tail", {"n": n})["lines"])


@server.tool()
def reload_core() -> dict:
    """Reload the probe core (out/nfsu2_core.dll) inside the running game. Rebuilding the core also triggers this
    automatically within ~0.5 s. Returns the new core's ping (build stamp, reload_count)."""
    return pc.reload()


@server.tool()
def keep_active(enabled: bool | None = None) -> dict:
    """Get/set whether the game keeps running when its window loses focus (probe patch, default on)."""
    return pc.call("keep_active", {} if enabled is None else {"enabled": enabled})


@server.tool()
def press(key: str, ms: int = 100) -> dict:
    """Press and release a key (DirectInput name like "enter", "up", "esc", "a", or a DIK code "0x1c").
    Held for at least `ms` and at least two game reads. Works while the game is unfocused."""
    return pc.key(key, "press", ms)


@server.tool()
def hold(key: str) -> dict:
    """Hold a key down until release()/release_all()."""
    return pc.key(key, "hold")


@server.tool()
def release(key: str) -> dict:
    """Release a held key."""
    return pc.key(key, "release")


@server.tool()
def release_all() -> dict:
    """Release every injected key."""
    return pc.call("release_all")


@server.tool()
def input_state() -> dict:
    """DirectInput devices the game uses (kind, read counts, pending injected events) and injected keys down."""
    return pc.call("input_state")


@server.tool()
def no_traffic(enabled: bool | None = None) -> dict:
    """EXPERIMENTAL, does NOT remove traffic: patches 0x42F910 to return (ZMenu's 'No Traffic' patch); traffic and
    roaming racers still drive with it on. Unknown side effects; don't use for tests."""
    return pc.call("no_traffic", {} if enabled is None else {"enabled": enabled})


@server.tool()
def car_state() -> dict:
    """Player car telemetry: position, forward/up/side axes, velocity, speed (m/s, km/h), gear (1=N, 2=1st), rpm,
    game state (3 frontend, 6 in game), frame time. Errors when not in game."""
    return pc.call("car_state")


@server.tool()
def drive_script(steps: list[dict], every: int = 30, fps: int = 60) -> dict:
    """Run a frame-locked drive script and return car checkpoints every `every` frames.
    steps: [{"key": "w", "from": 0.5, "to": 2.5}, {"key": "lshift", "at": 1.5}, ...] (seconds; keys by DIK name).
    Driving map: w/up throttle, s/down brake+reverse, a/d or left/right steer, space handbrake, lmenu nitrous,
    lshift shift up, lcontrol shift down (manual gearbox in the test save)."""
    import drive
    return drive.run_events(drive.script_events({"fps": fps, "steps": steps}), every)


@server.tool()
def record_inputs(name: str, seconds: float, every: int = 30) -> dict:
    """Record everything the game reads from the keyboard (real + injected, as game actions) plus car checkpoints
    for `seconds`, saved to tests/drives/NAME.json."""
    import json
    import time
    import drive
    pc.call("record_start", {"every": every})
    time.sleep(seconds)
    rec = pc.call("record_stop")
    drive.DRIVES.mkdir(parents=True, exist_ok=True)
    (drive.DRIVES / f"{name}.json").write_text(json.dumps(rec, indent=1))
    return {"saved": str(drive.DRIVES / f"{name}.json"), "events": len(rec["events"]), "frames": rec["frames"]}


@server.tool()
def replay_inputs(name: str) -> dict:
    """Replay tests/drives/NAME.json frame-locked and compare against its checkpoints (position error per checkpoint).
    Start from the same place as the recording (e.g. a fresh tools/run_game.sh --freeroam)."""
    import json
    import drive
    rec = json.loads((drive.DRIVES / f"{name}.json").read_text())
    last = max((e["f"] for e in rec["events"]), default=0)
    res = drive.run_events(rec["events"], rec["every"], tail=max(0, rec["frames"] - last))
    errs = drive.compare(rec["samples"], res["samples"])
    return {"checkpoint_errors_m": [{"frame": f, "error": round(e, 3)} for f, e in errs],
            "end_recorded": rec["samples"][-1] if rec["samples"] else None,
            "end_replayed": res["samples"][-1] if res["samples"] else None}


@server.tool()
def screenshot(scale: int = 2) -> Image:
    """Capture the game's back buffer (downscaled by integer `scale`). Also saved under work/screenshots/."""
    png, _, _ = pc.screenshot_png(scale)
    return Image(data=png, format="png")


if __name__ == "__main__":
    server.run()
