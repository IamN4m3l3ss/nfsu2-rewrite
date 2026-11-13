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
def screenshot(scale: int = 2) -> Image:
    """Capture the game's back buffer (downscaled by integer `scale`). Also saved under work/screenshots/."""
    png, _, _ = pc.screenshot_png(scale)
    return Image(data=png, format="png")


if __name__ == "__main__":
    server.run()
