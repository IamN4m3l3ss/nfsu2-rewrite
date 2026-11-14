#!/usr/bin/env python3
"""Telemetry, scripted driving, recording and replay (M5). Everything is frame-locked in the probe: events are
applied at Present of frame base+f and the car is sampled every N frames on the same base.

  drive.py state                          car_state once
  drive.py script FILE.json [--every N]   run a drive script, print checkpoints, save trace next to it
  drive.py record NAME SECONDS            record what the game reads from the keyboard (you drive), + checkpoints
  drive.py replay NAME                    replay a recording and compare against its checkpoints

Drive script JSON: {"fps": 60, "steps": [{"key": "w", "from": 0.0, "to": 4.0}, {"key": "lshift", "at": 1.5}]}
  "from"/"to": hold the key between those times (seconds); "at": tap (100 ms).
Recordings live in tests/drives/NAME.json: {"frames", "every", "events": [{f, action, down}], "samples": [...]}.
"""
from __future__ import annotations

import json
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dik  # noqa: E402
import probe_client as pc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DRIVES = ROOT / "tests" / "drives"


def car_state() -> dict:
    return pc.call("car_state")


def script_events(script: dict) -> list[dict]:
    fps = script.get("fps", 60)
    evs = []
    for st in script["steps"]:
        code = dik.code(st["key"])
        if "at" in st:
            f = round(st["at"] * fps)
            evs += [{"f": f, "key": code, "down": True}, {"f": f + max(2, round(0.1 * fps)), "key": code, "down": False}]
        else:
            evs += [{"f": round(st["from"] * fps), "key": code, "down": True},
                    {"f": round(st["to"] * fps), "key": code, "down": False}]
    return evs


def run_events(events: list[dict], every: int = 30, tail: int = 60) -> dict:
    """Schedule events (key or action), trace on the same base frame, wait until done + tail frames."""
    last = max((e["f"] for e in events), default=0)
    pc.call("schedule", {"events": events, "trace_every": every})
    while True:
        st = pc.call("schedule_status")
        if st["done"] and st["rel_frame"] >= last + tail:
            break
        time.sleep(0.05)
    pc.call("release_all")
    return pc.call("trace_stop")


def end_of(samples: list[dict]) -> dict:
    return samples[-1] if samples else {}


def dist(a: list[float], b: list[float]) -> float:
    return math.dist(a, b)


def compare(ref: list[dict], got: list[dict]) -> list[tuple[int, float]]:
    """Position error per common checkpoint frame."""
    by_f = {s["frame"]: s for s in got}
    return [(s["frame"], dist(s["pos"], by_f[s["frame"]]["pos"])) for s in ref if s["frame"] in by_f]


def fmt(s: dict) -> str:
    p = s["pos"]
    return (f"f={s['frame']:5d} pos=({p[0]:9.2f},{p[1]:9.2f},{p[2]:7.2f}) {s['speed_kmh']:6.1f} km/h "
            f"gear={s['gear']} rpm={s['rpm']:5.0f}")


def main(a: list[str]) -> int:
    if not a:
        print(__doc__)
        return 2
    try:
        if a[0] == "state":
            print(json.dumps(car_state(), indent=2))
        elif a[0] == "script":
            path = Path(a[1])
            every = int(a[a.index("--every") + 1]) if "--every" in a else 30
            res = run_events(script_events(json.loads(path.read_text())), every)
            for s in res["samples"]:
                print(fmt(s))
            out = path.with_suffix(".trace.json")
            out.write_text(json.dumps(res, indent=1))
            print("trace:", out)
        elif a[0] == "record":
            name, secs = a[1], float(a[2])
            pc.call("record_start", {"every": 30})
            print(f"recording {secs:.0f} s — drive now")
            time.sleep(secs)
            rec = pc.call("record_stop")
            DRIVES.mkdir(parents=True, exist_ok=True)
            (DRIVES / f"{name}.json").write_text(json.dumps(rec, indent=1))
            print(f"saved {DRIVES / (name + '.json')}: {len(rec['events'])} events, {rec['frames']} frames")
        elif a[0] == "replay":
            rec = json.loads((DRIVES / f"{a[1]}.json").read_text())
            res = run_events(rec["events"], rec["every"], tail=max(0, rec["frames"] - max((e["f"] for e in rec["events"]), default=0)))
            errs = compare(rec["samples"], res["samples"])
            for f, e in errs:
                print(f"f={f:5d} position error {e:7.2f} m")
            print(f"end: recorded {fmt(end_of(rec['samples']))}\n     replayed {fmt(end_of(res['samples']))}")
        else:
            print(__doc__)
            return 2
    except pc.ProbeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
