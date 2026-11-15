#!/usr/bin/env python3
"""Enable every rewrite a source file registers (NFSU2_REWRITE_* lines) in the running game and mark them verified
in docs/symbols.csv. Run only after the file's diff tests passed.   promote.py game/systems/x/file.cpp "how verified"
"""
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "probe"))
import probe_client as pc  # noqa: E402

src, note = sys.argv[1], sys.argv[2]
text = (ROOT / src).read_text()
regs = re.findall(r'NFSU2_REWRITE_\w+\(\s*(0x[0-9A-Fa-f]+),\s*\w+,\s*\d+,\s*"([^"]*)"', text)
regs += re.findall(r'Registrar \w+\((0x[0-9A-Fa-f]+),\s*"\w+",\s*"([^"]*)"', text)
rows = list(csv.DictReader((ROOT / "docs/symbols.csv").open()))
have = {int(r["addr"], 16): r for r in rows}
for a, sig in regs:
    pc.call("rewrite_enable", {"addr": a, "enabled": True}, timeout=20)
    m = re.search(r"([\w:~<>]+)\(", sig)
    name = m.group(1) if m else sig.split()[0]
    row = {"addr": f"0x{int(a, 16):08X}", "name": name, "signature": sig, "status": "verified",
           "notes": src.split("systems/")[-1] + "; " + note}
    if int(a, 16) in have:
        have[int(a, 16)].update(row)
    else:
        rows.append(row)
with (ROOT / "docs/symbols.csv").open("w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["addr", "name", "signature", "status", "notes"], lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
print(f"{len(regs)} enabled + recorded; verified now {sum(r['status'] == 'verified' for r in rows)}")
