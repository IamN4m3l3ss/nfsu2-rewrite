#!/usr/bin/env python3
"""M7 acceptance: every diff test in nfsu2_game.dll passes, and the deliberately broken one is caught.
Needs a running game with the probe (tools/run_game.sh). Exit code 0 = pass."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import probe_client as pc  # noqa: E402

N = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
SEEDS = [12345, 777, 2026]
ok = True
for seed in SEEDS:
    for r in pc.call("diff_test", {"name": "*", "iterations": N, "seed": seed}, timeout=120):
        broken = r["name"].endswith("_BROKEN")
        passed = (r["mismatches"] > 0) if broken else (r["mismatches"] == 0 and "error" not in r)
        ok &= passed
        print(f"{'PASS' if passed else 'FAIL'} seed={seed:5d} {r['name']:22s} {r['iterations']} cases, "
              f"{r['mismatches']} mismatches{' (expected: deliberately broken)' if broken else ''} {r.get('error', '')}")
        if not passed:
            for ex in r["examples"]:
                print("    ", ex)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
