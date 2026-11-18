#!/usr/bin/env python3
"""In-game A/B of camera rewrites: the same boot and the same frame-locked drive, once with the original camera code
and once with the rewrites, comparing the player camera (view matrix + eye position) every frame, bit for bit.

  cam_ab.py [--recording NAME|--script FILE] [--names N1,N2,...|--system camera] [--baseline-only]

--recording NAME: replay tests/drives/NAME.json (recorded with tools/probe/drive.py record NAME SECONDS from the
free-roam spawn point) instead of a drive script - a real driven route.

Each run: boot to free roam with the frame time pinned from the first frame (NFSU2_FIXED_DT=1), start the camera
recorder (probe `cam_record`), play the drive script (frame-locked input schedule), stop. Runs: orig_a, orig_b
(baseline: must be identical, else the setup is not deterministic) and rewrites. The rewrites under test are
switched on from the first frame through out/rewrites_enabled.json; everything else stays as normally enabled.
Default drive (tests/drives/camera_ab.script.json): throttle, steering both ways, braking, and C presses that cycle
the camera views (bumper, hood, chase near/far) so several camera movers run.
Recordings: work/cam_ab/<run>.json. Restarts the game 3 times; leaves it in free roam with the normal set.
"""
import json
import statistics
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "probe"))
import drive  # noqa: E402
import probe_client as pc  # noqa: E402

OUT = ROOT / "work" / "cam_ab"
ENABLED = ROOT / "out" / "rewrites_enabled.json"
DEFAULT_SCRIPT = ROOT / "tests" / "drives" / "camera_ab.script.json"
DEFAULT_DRIVE = {"fps": 60, "steps": [
    {"key": "w", "from": 0.5, "to": 6.0},
    {"key": "a", "from": 2.0, "to": 3.0},
    {"key": "d", "from": 3.6, "to": 4.6},
    {"key": "c", "at": 3.0}, {"key": "c", "at": 5.0}, {"key": "c", "at": 7.0}, {"key": "c", "at": 9.0},
    {"key": "s", "from": 6.0, "to": 8.0},
    {"key": "w", "from": 9.0, "to": 11.0},
]}


def rewrites():
    return pc.call("rewrite_list")["rewrites"]


def run(label, enabled_names, events, tested=frozenset(), dt="1"):
    ENABLED.write_text(json.dumps(sorted(enabled_names)))
    env = dict(os.environ, NFSU2_FIXED_DT=dt, NFSU2_NO_TRAFFIC="1")  # pinned frame time, no traffic cars
    t = time.monotonic()
    r = subprocess.run([str(ROOT / "nfsu2"), "freeroam"], capture_output=True, text=True, env=env)
    if "free roam after" not in r.stdout:
        print(f"  {label}: BOOT FAILED\n{(r.stdout + r.stderr)[-500:]}")
        return None
    pc.call("cam_record", {"action": "start", "view": 1})
    tail = 60
    trace = drive.run_events(events, every=10, tail=tail, time_sync=True)
    base = pc.call("schedule_status")["base_frame"]
    rec = pc.call("cam_record", {"action": "stop"}, timeout=60)
    # the same window in every run: from the drive's first frame to its end (inputs are frame-locked to `base`)
    span = max((e["f"] for e in events), default=0) + tail
    rec["frames"] = [h for h in rec["frames"] if 0 <= int(h[152:160], 16) - (base & 0xFFFFFFFF) < span]
    rec["count"] = len(rec["frames"])
    ran = sorted(x["name"] for x in rewrites() if x["enabled"] and x.get("calls", 0) and x["name"] in tested)
    (OUT / f"{label}.json").write_text(json.dumps({"frames": rec["frames"], "ran": ran, "car": trace.get("samples", [])}))
    print(f"  {label}: {rec['count']} frames ({time.monotonic() - t:.0f}s)", flush=True)
    return rec["frames"]


def decode(h):
    w = [int(h[i:i + 8], 16) for i in range(0, 160, 8)]
    return w[:19], w[19]


def compare(a, b, name):
    n = min(len(a), len(b))
    first, diffs, worst = None, 0, 0.0
    for i in range(n):
        wa, _ = decode(a[i])
        wb, _ = decode(b[i])
        if wa != wb:
            diffs += 1
            if first is None:
                first = i
            for x, y in zip(wa, wb):
                fx, fy = struct.unpack("<f", struct.pack("<I", x))[0], struct.unpack("<f", struct.pack("<I", y))[0]
                if fx == fx and fy == fy:
                    worst = max(worst, abs(fx - fy))
    same_len = len(a) == len(b)
    verdict = "IDENTICAL" if diffs == 0 and same_len else f"{diffs} of {n} frames differ (first at frame {first}, " \
        f"largest difference {worst:.6g}){'' if same_len else f'; lengths {len(a)} / {len(b)}'}"
    print(f"  {name}: {verdict}")
    return diffs == 0 and same_len


def main(argv):
    OUT.mkdir(parents=True, exist_ok=True)
    script = DEFAULT_SCRIPT
    names, system, baseline_only, recording = None, None, False, None
    dt = "1"
    i = 0
    while i < len(argv):
        if argv[i] == "--script":
            script, i = Path(argv[i + 1]), i + 2
        elif argv[i] == "--names":
            names, i = set(argv[i + 1].split(",")), i + 2
        elif argv[i] == "--system":
            system, i = argv[i + 1], i + 2
        elif argv[i] == "--recording":  # a drive recorded with tools/probe/drive.py record NAME SECONDS
            recording, i = argv[i + 1], i + 2
        elif argv[i] == "--baseline-only":
            baseline_only, i = True, i + 1
        else:
            print(__doc__)
            return 2
    if not script.exists():
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text(json.dumps(DEFAULT_DRIVE, indent=1))
    if recording:
        rec = json.loads((drive.DRIVES / f"{recording}.json").read_text())
        events = rec["events"]
        dts = [x["dt"] for x in rec.get("samples", []) if x.get("dt")]
        if dts:  # replay at the frame time it was driven with: same speed, same path
            dt = repr(statistics.median(dts))
        script = Path(f"{recording}.json (recorded)")
    else:
        events = drive.script_events(json.loads(script.read_text()))
    backup = ENABLED.read_text() if ENABLED.exists() else "[]"
    try:
        pc.call("ping")
    except pc.ProbeError:
        subprocess.run([str(ROOT / "nfsu2"), "freeroam"], capture_output=True)
    rl = rewrites()
    normal = set(json.loads(backup))
    if system:
        names = {x["name"] for x in rl if x.get("system") == system}
    names = names or set()
    print(f"{len(names)} rewrites under test; drive {script.name}, {len(events)} input events", flush=True)
    try:
        a = run("orig_a", normal - names, events, dt=dt)
        b = run("orig_b", normal - names, events, dt=dt)
        t = None if baseline_only else run("rewrites", normal | names, events, frozenset(names), dt=dt)
    finally:
        ENABLED.write_text(backup)
        subprocess.run([str(ROOT / "nfsu2"), "freeroam"], capture_output=True)
    if not (a and b):
        return 2
    print("comparison (player camera: view matrix + eye position, bit for bit):")
    base_ok = compare(a, b, "baseline orig_a vs orig_b")
    if baseline_only:
        return 0 if base_ok else 1
    if not t:
        print("  rewrites: BOOT FAILED (crash?) - see work/logs/game_latest.log")
        return 1
    ok = compare(a, t, "rewrites vs orig_a")
    ran = json.loads((OUT / "rewrites.json").read_text())["ran"]
    print(f"{len(ran)} of the {len(names)} rewrites under test ran during the drive"
          + (f": {', '.join(ran[:12])}{' ...' if len(ran) > 12 else ''}" if ran else ""))
    if not base_ok:
        print("baseline differs: the setup is not deterministic, the rewrite comparison means nothing yet")
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
