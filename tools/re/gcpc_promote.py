#!/usr/bin/env python3
"""Promote UNVERIFIED nfsu2-gcpc rewrites to verified after an A/B run proved them.

  gcpc_promote.py CALLED_JSON "how they were verified"
CALLED_JSON: {addr: name} (tools/game/fe_boot_ab.py writes work/fe_ab/called.json). Moves the addresses from
game/systems/frontend/gcpc_hooked.inc to gcpc_allow.inc and marks them verified in docs/symbols.csv.
Only promote after the A/B said IDENTICAL and the HUD/pause screenshots match the original.
"""
import csv
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ALLOW = ROOT / "game/systems/frontend/gcpc_allow.inc"
HOOKED = ROOT / "game/systems/frontend/gcpc_hooked.inc"
SYMBOLS = ROOT / "docs/symbols.csv"


def load(p):
    return {int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", "".join(l for l in p.open() if not l.startswith("//")))}


def save(p, addrs):
    hdr = "".join(l for l in p.open() if l.startswith("//"))
    al = sorted(addrs)
    with p.open("w") as f:
        f.write(hdr)
        for i in range(0, len(al), 8):
            f.write("    " + ", ".join(f"0x{a:06X}" for a in al[i:i + 8]) + ",\n")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    called = {int(a, 16): n for a, n in json.loads(Path(argv[0]).read_text()).items()}
    hooked = load(HOOKED)
    missing = [hex(a) for a in called if a not in hooked]
    if missing:
        print(f"not in the UNVERIFIED list (already promoted or excluded): {missing}")
    todo = {a: n for a, n in called.items() if a in hooked}
    save(HOOKED, hooked - set(todo))
    save(ALLOW, load(ALLOW) | set(todo))
    note = f"gcpc:frontend; nfsu2-gcpc rewrite, registered by frontend/gcpc_hooks.cpp; {argv[1]}"
    rows = list(csv.DictReader(SYMBOLS.open()))
    have = {int(r["addr"], 16): r for r in rows}
    for a, n in todo.items():
        if a in have:
            have[a].update(status="verified", notes=note)
        else:
            rows.append({"addr": f"0x{a:08X}", "name": n, "signature": "nfsu2-gcpc rewrite", "status": "verified",
                         "notes": note})
    with SYMBOLS.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["addr", "name", "signature", "status", "notes"], lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    print(f"promoted {len(todo)}; verified now {sum(r['status'] == 'verified' for r in rows)}, "
          f"UNVERIFIED left {len(hooked) - len(todo)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
