#!/usr/bin/env python3
"""Checkpoint A/B of menu (FEng) rewrites over the whole boot: title -> profile load -> main menu -> career ->
garage -> explore dialog -> free roam (HUD).

At every screen tools/game/boot_freeroam.py confirms, every menu object's fingerprint is saved (NFSU2_FE_DUMP).
Two boots with the original code give the noise mask (objects that legitimately differ: looping animations, the
minimap, ...). A boot with the UNVERIFIED nfsu2-gcpc rewrites on (enabled from the first frame through
out/rewrites_enabled.json) must match everything outside the mask. Each run also saves a HUD screenshot after
pause/unpause (work/fe_ab/<label>_hud.png): look at it, the fingerprint doesn't see what mods do at render time.

  fe_boot_ab.py [--walk NAME] [--only ADDR,ADDR,...]   (default: boot to free roam, all UNVERIFIED rewrites)
--walk NAME: boot only to the main menu, then press the keys of WALKS[NAME] (each followed by a settle wait and a
checkpoint); every walk puts all settings back the way it found them and ends on the main menu.
Restarts the game three times; leaves it in free roam with the normal enabled set.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "probe"))
import probe_client as pc  # noqa: E402

OUT = ROOT / "work" / "fe_ab"
ENABLED = ROOT / "out" / "rewrites_enabled.json"


# (key, settle seconds). Car select: changing the car loads its model, so wait longer.
WALKS = {
    "quickrace": [("enter", 2.5), ("right", 1.5), ("left", 1.5), ("enter", 2.5),           # mode select -> track
                  ("right", 1.5), ("right", 1.5), ("left", 1.5), ("left", 1.5), ("enter", 2.5),  # tracks -> options
                  ("down", 1.0), ("down", 1.0), ("down", 1.0), ("up", 1.0), ("up", 1.0), ("up", 1.0),
                  ("right", 1.0), ("left", 1.0), ("enter", 3.5),                              # direction; -> car select
                  ("right", 3.5), ("left", 3.5),
                  ("esc", 2.5), ("esc", 2.5), ("esc", 2.5), ("esc", 2.5)],                   # back to the main menu
    # Options (5x right of Quick Race): Audio, Display, Gameplay, Player (browse only), Controls, EA Trax; no Credits
    "options": [("right", 1.2)] * 5 + [("enter", 2.5)]
               + ([("enter", 2.5)] + [("down", 1.0)] * 3 + [("up", 1.0)] * 3 + [("esc", 2.5), ("right", 1.2)]) * 4
               + [("enter", 2.5), ("esc", 2.5), ("right", 1.2)]
               + [("enter", 2.5), ("down", 1.0), ("down", 1.0), ("up", 1.0), ("up", 1.0), ("esc", 2.5)]
               + [("esc", 2.5)] + [("left", 1.2)] * 5,
}
WALK = None
# Packages driven by real-time timers, never comparable: the EA Trax "now playing" banner (shown after the boot,
# fades out a few seconds later; its phase depends on how long the boot took).
NOISE_PACKAGES = {"1f457265"}


def rewrites():
    return pc.call("rewrite_list")["rewrites"]


def boot(label, enabled_names):
    ENABLED.write_text(json.dumps(sorted(enabled_names)))
    dump = OUT / f"{label}.json"
    dump.unlink(missing_ok=True)
    env = dict(os.environ, NFSU2_FE_DUMP=str(dump))
    if WALK:
        env["BOOT_STOP_AT"] = "main"
    t = time.monotonic()
    r = subprocess.run([str(ROOT / "nfsu2"), "freeroam"], capture_output=True, text=True, env=env)
    ok = ("stopped at main" if WALK else "free roam after") in r.stdout
    if ok and WALK:
        dumps = json.loads(dump.read_text())
        for i, (k, settle) in enumerate(WALKS[WALK]):
            pc.key(k, "press", 100)
            time.sleep(settle)
            dumps[f"{i + 1:02d}_{k}"] = pc.call("fe_state", {"objects": True}, timeout=10)["objects"]
        dump.write_text(json.dumps(dumps))
    print(f"  {label}: {'ok' if ok else 'BOOT FAILED'} ({time.monotonic() - t:.0f}s)", flush=True)
    if not ok:
        print((r.stdout + r.stderr)[-600:])
    called = {}
    if ok:
        called = {x["addr"]: x["name"] for x in rewrites() if x["enabled"] and x.get("calls", 0)
                  and x["signature"].startswith("UNVERIFIED")}
        for _ in range(0 if WALK else 2):  # pause/unpause refreshes the HUD layout
            pc.key("esc", "press", 100)
            time.sleep(2.5)
        png, _, _ = pc.screenshot_png(2, save=False)
        (OUT / f"{label}_hud.png").write_bytes(png)
    dumps = json.loads(dump.read_text()) if dump.exists() else {}
    return ok, dumps, called


def by_path(objs):
    return {o[0]: (o[1], o[2], o[3]) for o in objs}


def main(argv):
    OUT.mkdir(parents=True, exist_ok=True)
    backup = ENABLED.read_text() if ENABLED.exists() else "[]"
    if subprocess.run([str(ROOT / "nfsu2"), "status"], capture_output=True).returncode != 0:
        subprocess.run([str(ROOT / "nfsu2"), "freeroam"], capture_output=True)
    rl = rewrites()
    global WALK
    if argv[:1] == ["--walk"]:
        WALK, argv = argv[1], argv[2:]
    normal = set(json.loads(backup))
    unverified = {x["addr"]: x["name"] for x in rl if x["signature"].startswith("UNVERIFIED")}
    if argv[:1] == ["--only"]:
        want = {int(a, 16) for a in argv[1].split(",")}
        unverified = {a: n for a, n in unverified.items() if int(a, 16) in want}
    normal -= set(unverified.values())
    print(f"{len(unverified)} UNVERIFIED under test, {len(normal)} normally enabled", flush=True)
    try:
        ok_a, a, _ = boot("orig_a", normal)
        ok_b, b, _ = boot("orig_b", normal)
        ok_t, t, called = boot("rewrites", normal | set(unverified.values()))
    finally:
        ENABLED.write_text(backup)
        try:
            for x in rewrites():
                if x["enabled"] and x["signature"].startswith("UNVERIFIED"):
                    pc.call("rewrite_enable", {"addr": x["addr"], "enabled": False}, timeout=20)
        except pc.ProbeError:
            pass
    if not (ok_a and ok_b):
        print("baseline boot failed")
        return 2
    if not ok_t:
        print("BOOT WITH REWRITES FAILED (crash/hang?) - see work/logs/game_latest.log")
        return 1
    bad = 0
    # noise learned from every baseline pair so far (the free roam minimap markers depend on where the car settles)
    nf = OUT / f"noise_{WALK or 'boot'}.json"
    known = json.loads(nf.read_text()) if nf.exists() else {}
    for cp in a:
        pa, pb, pt = by_path(a[cp]), by_path(b.get(cp, [])), by_path(t.get(cp, []))
        noise = {p for p in set(pa) | set(pb) if pa.get(p) != pb.get(p)} | set(known.get(cp, []))
        noise |= {p for p in set(pa) | set(pb) | set(pt) if p.split("/")[0] in NOISE_PACKAGES}
        known[cp] = sorted(noise)
        diff = sorted(p for p in set(pa) | set(pt) if p not in noise and pa.get(p) != pt.get(p))

        def near_noise(p):  # same parent, list index within 2 of a noisy object (a family like the map markers)
            head, _, idx = p.rpartition("/")
            return idx.lstrip("-").isdigit() and any(f"{head}/{int(idx) + d}" in noise for d in (-2, -1, 1, 2))
        likely = [p for p in diff if cp == "freeroam" and near_noise(p)]
        real = [p for p in diff if p not in likely]
        print(f"  {cp:14s} objects {len(pa):4d}  noise {len(noise):3d}  DIFF {len(real)}"
              + (f"  e.g. {real[:4]}" if real else "") + (f"  (likely noise: {likely})" if likely else ""))
        bad += len(real)
    nf.write_text(json.dumps(known))
    for cp in set(t) - set(a):
        print(f"  {cp}: only in the rewrite run")
        bad += 1
    print(f"{len(called)} UNVERIFIED rewrites ran during the boot")
    (OUT / "called.json").write_text(json.dumps(called, indent=0))
    print("IDENTICAL (outside noise) - now compare work/fe_ab/orig_a_hud.png with rewrites_hud.png" if not bad
          else f"{bad} DIFFERENCES")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
