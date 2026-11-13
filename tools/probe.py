#!/usr/bin/env python3
"""Command-line access to the probe (same calls as the MCP bridge).

  tools/probe.py ping | info
  tools/probe.py read ADDR [SIZE]
  tools/probe.py write ADDR HEX
  tools/probe.py scan "8B 0D ?? ?? ?? ??" [--max N]
  tools/probe.py log [N]
  tools/probe.py shot [SCALE]          -> saves PNG under work/screenshots/, prints the path
  tools/probe.py press KEY [MS] | hold KEY | release KEY | release_all | input
                                       -> DirectInput key injection (names from tools/dik.py, e.g. enter, up, esc)
  tools/probe.py wait [TIMEOUT]        -> wait until the probe answers
  tools/probe.py ready [TIMEOUT]       -> wait until the probe answers and frames are being presented
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
        elif cmd == "shot-wait":
            # screenshot repeatedly until the image is not (near) black: proves the game drew real content
            import io, time
            from PIL import Image, ImageStat
            scale = int(args[0]) if args else 4
            end = time.monotonic() + (float(args[1]) if len(args) > 1 else 30.0)
            while True:
                png, meta, path = pc.screenshot_png(scale)
                mean = sum(ImageStat.Stat(Image.open(io.BytesIO(png))).mean) / 3
                if mean > 12:
                    print(path, json.dumps({**meta, "mean": round(mean, 1)}))
                    break
                path.unlink()
                if time.monotonic() > end:
                    raise pc.ProbeError(f"still black after timeout (mean {mean:.1f}, frame {meta['frame']})")
                time.sleep(0.5)
        elif cmd in ("press", "hold", "release"):
            ms = int(args[1]) if len(args) > 1 else 100
            print(json.dumps(pc.key(args[0], cmd, ms)))
        elif cmd == "release_all":
            print(json.dumps(pc.call("release_all")))
        elif cmd == "input":
            print(json.dumps(pc.call("input_state"), indent=2))
        elif cmd == "wait":
            print(json.dumps(pc.wait_ready(float(args[0]) if args else 60.0)))
        elif cmd == "ready":
            print(json.dumps(pc.wait_frames(float(args[0]) if args else 60.0)))
        else:
            print(__doc__)
            return 2
    except pc.ProbeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
