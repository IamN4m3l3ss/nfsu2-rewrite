#!/usr/bin/env python3
"""End-to-end check of tools/mcp_bridge.py through a real MCP stdio client (mcp SDK 2.3.0).
Needs the game running with the probe. Exit code 0 = all M1 checks passed."""
import asyncio
import base64
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters, stdio_client

ROOT = Path(__file__).resolve().parent.parent


async def main() -> int:
    params = StdioServerParameters(command=sys.executable, args=[str(ROOT / "tools" / "mcp_bridge.py")])
    fails = 0
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = sorted(t.name for t in (await session.list_tools()).tools)
            print("tools:", tools)
            expected = {"ping", "info", "read_mem", "write_mem", "aob_scan", "log_tail", "screenshot"}
            if not expected <= set(tools):
                print("FAIL missing tools", expected - set(tools)); fails += 1

            r = await session.call_tool("read_mem", {"addr": "0x400000", "size": 2})
            text = r.content[0].text
            ok = "4d 5a" in text
            print("PASS" if ok else "FAIL", "exe header MZ at 0x400000:", text.strip()); fails += not ok

            r = await session.call_tool("read_mem", {"addr": "0x7a0a30", "size": 13})
            ok = "TRACKS\\%s.BUN" in r.content[0].text
            print("PASS" if ok else "FAIL", "constant TRACKS\\%s.BUN at 0x7a0a30"); fails += not ok

            r = await session.call_tool("screenshot", {"scale": 4})
            img = next((c for c in r.content if c.type == "image"), None)
            ok = img is not None and base64.b64decode(img.data)[:8] == b"\x89PNG\r\n\x1a\n"
            print("PASS" if ok else "FAIL", "screenshot returns a PNG image",
                  f"({len(base64.b64decode(img.data))} bytes)" if img else r.content); fails += not ok
    print(f"{fails} failure(s)")
    return fails


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
