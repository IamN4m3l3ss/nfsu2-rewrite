#!/usr/bin/env python3
"""Command-line access to the probe (same calls as the MCP bridge).

  tools/probe.py ping | info
  tools/probe.py read ADDR [SIZE]
  tools/probe.py write ADDR HEX
  tools/probe.py scan "8B 0D ?? ?? ?? ??" [--max N]
  tools/probe.py log [N]
  tools/probe.py shot [SCALE]          -> saves PNG under work/screenshots/, prints the path
  tools/probe.py wait [TIMEOUT]        -> wait until the probe answers
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import probe_client as pc  # noqa: E402


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    cmd, args = argv[0], argv[1:]
    try:
        if cmd in ("ping", "info"):
            print(json.dumps(pc.call(cmd), indent=2))
        elif cmd == "read":
            addr = int(args[0], 0)
            size = int(args[1], 0) if len(args) > 1 else 64
            r = pc.call("read_mem", {"addr": addr, "size": size})
            print(pc.hexdump(addr, bytes.fromhex(r["hex"])))
        elif cmd == "write":
            print(json.dumps(pc.call("write_mem", {"addr": int(args[0], 0), "hex": args[1]})))
        elif cmd == "scan":
            mx = int(args[args.index("--max") + 1]) if "--max" in args else 64
            print(json.dumps(pc.call("aob_scan", {"pattern": args[0], "max": mx}), indent=2))
        elif cmd == "log":
            n = int(args[0]) if args else 50
            for line in pc.call("log_tail", {"n": n})["lines"]:
                print(line["text"])
        elif cmd == "shot":
            scale = int(args[0]) if args else 2
            _, meta, path = pc.screenshot_png(scale)
            print(path, json.dumps(meta))
        elif cmd == "wait":
            print(json.dumps(pc.wait_ready(float(args[0]) if args else 60.0)))
        else:
            print(__doc__)
            return 2
    except pc.ProbeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
