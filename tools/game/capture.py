#!/usr/bin/env python3
"""Record the real inputs of a game function during play, for replay in its diff test (leaf::run_captured).

  capture.py ADDR [--nargs N] [--this BYTES] [--args S1,S2,...] [--count N] [--drive|--recording NAME] [--seconds S]

ADDR: function address (hex). --nargs: stack arguments; --this: bytes of `this` to snapshot (thiscall; 0 = none);
--args: snapshot size per argument (0 = a value, else the bytes behind that pointer). Records up to --count calls
(default 2000) while you play (--seconds S), or during the A/B camera drive (--drive: reboots into free roam first,
so the drive starts at the spawn point).
The function's rewrite hook is removed while capturing and restored after. Writes work/captures/<ADDR>.cap
(gitignored: it holds live game memory).
"""
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "probe"))
import drive  # noqa: E402
import probe_client as pc  # noqa: E402


def main(argv):
    if not argv or argv[0].startswith("-"):
        print(__doc__)
        return 2
    addr = int(argv[0], 16)
    opt = {"nargs": 0, "this": 0, "args": "", "count": 2000, "seconds": 20.0}
    use_drive, recording = False, None
    i = 1
    while i < len(argv):
        k = argv[i].lstrip("-")
        if k == "drive":
            use_drive, i = True, i + 1
        elif k == "recording":
            use_drive, recording, i = True, argv[i + 1], i + 2
        elif k in opt:
            v = argv[i + 1]
            opt[k] = v if isinstance(opt[k], str) else (float(v) if isinstance(opt[k], float) else int(v, 0))
            i += 2
        else:
            print(__doc__)
            return 2
    sizes = [int(x, 0) for x in opt["args"].split(",")] if opt["args"] else []
    sizes += [0] * (opt["nargs"] - len(sizes))
    out = ROOT / "work" / "captures"
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{addr:06X}.cap"
    winpath = subprocess.run(["winepath", "-w", str(path)], capture_output=True, text=True).stdout.strip()
    if use_drive:  # the scripted drive only makes sense from the free-roam spawn point: fresh boot first
        import os
        env = dict(os.environ)
        if recording:  # replay at the frame time it was driven with: same speed, same path
            dts = [x["dt"] for x in json.loads((drive.DRIVES / f"{recording}.json").read_text()).get("samples", []) if x.get("dt")]
            if dts:
                env["NFSU2_FIXED_DT"] = repr(statistics.median(dts))
        r = subprocess.run([str(ROOT / "nfsu2"), "freeroam", "--no-traffic"], capture_output=True, text=True, env=env)
        if "free roam after" not in r.stdout:
            print("boot to free roam failed:\n" + (r.stdout + r.stderr)[-400:])
            return 1
    pc.call("capture", {"action": "start", "addr": hex(addr), "nargs": opt["nargs"], "this_size": opt["this"],
                        "arg_sizes": sizes, "count": opt["count"], "path": winpath})
    try:
        if use_drive:
            if recording:
                events = json.loads((drive.DRIVES / f"{recording}.json").read_text())["events"]
            else:
                events = drive.script_events(json.loads((ROOT / "tests" / "drives" / "camera_ab.script.json").read_text()))
            drive.run_events(events, every=60, tail=60)
        else:
            end = time.monotonic() + opt["seconds"]
            while time.monotonic() < end:
                st = pc.call("capture", {"action": "status"})
                if st["left"] == 0:
                    break
                time.sleep(0.5)
    finally:
        r = pc.call("capture", {"action": "stop"}, timeout=30)
    print(f"{r['count']} calls of 0x{addr:06X} recorded -> {path.relative_to(ROOT)} ({r['bytes']:,} bytes)")
    return 0 if r["count"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
