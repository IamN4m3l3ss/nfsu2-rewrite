#!/usr/bin/env python3
"""A/B test for menu (FEng) rewrites: the same scripted menu walk, frame-locked, from a fresh boot into free roam,
recording the menu-engine fingerprint (probe fe_state) every frame. Two runs must match frame for frame.

  fe_ab.py baseline            original vs original (is the walk deterministic at all?)
  fe_ab.py test                original vs the UNVERIFIED nfsu2-gcpc rewrites switched on
Writes work/fe_ab/<label>.json; prints the first differing frame and which UNVERIFIED rewrites ran.
Restarts the game for every run (it must not be in use).
"""
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "probe"))
import probe_client as pc  # noqa: E402

OUT = ROOT / "work" / "fe_ab"
ESC, UP, DOWN, LEFT, RIGHT, ENTER = 0x01, 0xC8, 0xD0, 0xCB, 0xCD, 0x1C

# pause menu walk: open, move around, open sub-menus, back out, resume
WALK = [(0, ESC)] + [(90 + 30 * i, k) for i, k in enumerate(
    [DOWN, DOWN, UP, DOWN, ENTER, DOWN, DOWN, RIGHT, LEFT, UP, ESC, DOWN, DOWN, DOWN, ENTER, DOWN, UP, ESC, UP, UP, UP])]
FRAMES = WALK[-1][0] + 120


def events():
    ev = []
    for f, k in WALK:
        ev.append({"f": f, "key": k, "down": True})
        ev.append({"f": f + 4, "key": k, "down": False})
    return ev


def unverified():
    return [x for x in pc.call("rewrite_list")["rewrites"] if x["signature"].startswith("UNVERIFIED")]


def run(label, rewrites_on):
    subprocess.run([str(ROOT / "nfsu2"), "kill"], capture_output=True)
    r = subprocess.run([str(ROOT / "nfsu2"), "freeroam"], capture_output=True, text=True)
    if "free roam after" not in r.stdout:
        raise SystemExit(f"boot failed: {r.stdout[-300:]}")
    for x in unverified():
        if x["enabled"] != rewrites_on:
            pc.call("rewrite_enable", {"addr": x["addr"], "enabled": rewrites_on}, timeout=20)
    time.sleep(3)
    pc.call("fixed_dt", {"enabled": True, "dt": 1 / 60})  # repeatable menu timing (frame hitches shift animations)
    pc.call("schedule", {"events": events(), "trace_every": 1, "fe": True})
    while True:
        st = pc.call("schedule_status")
        if st["done"] and st["rel_frame"] > FRAMES:
            break
        time.sleep(0.5)
    samples = pc.call("trace_stop")["samples"]
    pc.call("fixed_dt", {"enabled": False})
    # per frame: the packages that were NOT active at frame 0 (the menus the walk opened; the live HUD is ignored)
    first = set(samples[0].get("fe_pk", {})) if samples else set()
    seq = [(s["frame"], json.dumps({k: v for k, v in sorted(s.get("fe_pk", {}).items()) if k not in first}), 0)
           for s in samples if s["frame"] <= FRAMES]
    called = {x["name"]: x.get("calls", 0) for x in unverified() if x.get("calls", 0)} if rewrites_on else {}
    for x in unverified():  # leave the game with them off
        if x["enabled"]:
            pc.call("rewrite_enable", {"addr": x["addr"], "enabled": False}, timeout=20)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f"{label}.json").write_text(json.dumps({"seq": seq, "called": called}))
    return seq, called


def compare(a, b):
    da = {f: (h, n) for f, h, n in a}
    db = {f: (h, n) for f, h, n in b}
    common = sorted(set(da) & set(db))
    for f in common:
        if da[f] != db[f]:
            return f, da[f], db[f], len(common)
    return None, None, None, len(common)


def main(argv):
    mode = argv[0] if argv else "baseline"
    a, _ = run("orig_a", False)
    b, called = run("orig_b" if mode == "baseline" else "rewrites", mode != "baseline")
    f, x, y, n = compare(a, b)
    print(f"{n} frames compared; distinct states: A {len(set(h for _, h, _ in a))}, B {len(set(h for _, h, _ in b))}")
    print("IDENTICAL" if f is None else f"FIRST DIFFERENCE at frame {f}: {x} vs {y}")
    if called:
        print(f"{len(called)} UNVERIFIED rewrites ran: " + ", ".join(sorted(called)[:12]) + (" ..." if len(called) > 12 else ""))
    return 0 if f is None else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
