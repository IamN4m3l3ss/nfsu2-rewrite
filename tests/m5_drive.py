#!/usr/bin/env python3
"""M5 acceptance: a scripted drive along a known road ends within tolerance of the recorded end position.

  A: boot to free roam, record while tests/drives/straight.script.json drives  -> tests/drives/m5_ref.json
  B: reboot, replay the recording (game actions, frame-locked)               -> compare checkpoints
  C: reboot, run the key script again                                          -> compare checkpoints
Pass: end position of B and C within TOL metres of A. Exit code 0 = pass.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import drive  # noqa: E402
import probe_client as pc  # noqa: E402

TOL = 1.0
SCRIPT = ROOT / "tests" / "drives" / "straight.script.json"
REF = ROOT / "tests" / "drives" / "m5_ref.json"


def boot() -> None:
    r = subprocess.run([str(ROOT / "tools" / "run_game.sh"), "--freeroam"], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"boot failed: {r.stdout[-300:]} {r.stderr[-300:]}")
    time.sleep(1.0)  # car settled at the spawn


def wait_rel(frames: int) -> None:
    base = pc.call("ping")["frames"]
    while pc.call("ping")["frames"] < base + frames:
        time.sleep(0.05)


def report(name: str, ref: list, got: list) -> float:
    errs = drive.compare(ref, got)
    worst = max(e for _, e in errs)
    end = errs[-1][1]
    print(f"{name}: {len(errs)} checkpoints, worst {worst:.3f} m, end {end:.3f} m")
    print(f"   ref {drive.fmt(ref[-1])}\n   got {drive.fmt(got[-1])}")
    return end


def main() -> int:
    script = json.loads(SCRIPT.read_text())
    events = drive.script_events(script)
    last = max(e["f"] for e in events)

    boot()
    pc.call("record_start", {"every": 30})
    pc.call("schedule", {"events": events})
    while not pc.call("schedule_status")["done"]:
        time.sleep(0.05)
    wait_rel(60)
    rec = pc.call("record_stop")
    REF.write_text(json.dumps(rec, indent=1))
    print(f"A: recorded {len(rec['events'])} events over {rec['frames']} frames, end {drive.fmt(rec['samples'][-1])}")

    boot()
    res_b = drive.run_events(rec["events"], rec["every"], tail=rec["frames"] - max(e["f"] for e in rec["events"]))
    end_b = report("B replay", rec["samples"], res_b["samples"])

    boot()
    # the script starts one frame after record_start in run A; offset events so frames line up with A's trace
    off = min(e["f"] for e in rec["events"]) - min(e["f"] for e in events)
    res_c = drive.run_events([{**e, "f": e["f"] + off} for e in events], rec["every"],
                             tail=rec["frames"] - (last + off))
    end_c = report("C script", rec["samples"], res_c["samples"])

    ok = end_b <= TOL and end_c <= TOL
    print(f"{'PASS' if ok else 'FAIL'} (tolerance {TOL} m)")
    subprocess.run([str(ROOT / "tools" / "run_game.sh"), "--kill"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
