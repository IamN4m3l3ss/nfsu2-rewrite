#!/usr/bin/env python3
"""Everything we know about a SPEED2.EXE function, in one place.

  kb.py ADDR [ADDR...]            summary: our symbols.csv row, knowledge.db matches/facts/nfsu2-gcpc status,
                                  where it is in the PC and GameCube decompilations
  kb.py ADDR --pc                 + the PC decompilation of the function
  kb.py ADDR --gc                 + the GameCube decompilation of its best match
  kb.py find TEXT                 search names (ours, PC dump, GC dump) for TEXT (case-insensitive)
  kb.py gc GCADDR                 the GameCube decompilation at a GameCube address (e.g. a better match found by
                                  `find` than the db's automatic one)

Sources (paths from .env.local): docs/symbols.csv, KNOWLEDGE_DB (nfsu2-gcpc, CC0), PC_DECOMP and GC_DECOMP
(Ghidra exports in the RE kit, outside the repo - never commit them). Names from knowledge.db are hints only:
auto matches below ~0.8 are often wrong; a function counts only after our own diff test.
"""
import csv
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def env():
    out = {}
    for line in (ROOT / ".env.local").read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip().strip('"')
    return out


ENV = env()
DB = ENV.get("KNOWLEDGE_DB")
DUMPS = {"pc": ENV.get("PC_DECOMP"), "gc": ENV.get("GC_DECOMP")}


def db():
    return sqlite3.connect(f"file:{DB}?mode=ro", uri=True) if DB and Path(DB).exists() else None


def our_symbols():
    with open(ROOT / "docs" / "symbols.csv", newline="") as f:
        return {int(r["addr"], 16): r for r in csv.DictReader(f)}


def dump_index(which):
    d = DUMPS.get(which)
    p = Path(d) / "functions.tsv" if d else None
    if not p or not p.exists():
        return {}
    idx = {}
    with open(p) as f:
        next(f)
        for line in f:
            a, file, name = line.rstrip("\n").split("\t")
            idx[int(a, 16)] = (file, name)
    return idx


def dump_code(which, addr):
    """The function's text from its .c file: from its '// name @ addr' marker to the next marker."""
    idx = dump_index(which)
    if addr not in idx:
        return None
    path = Path(DUMPS[which]) / "src" / idx[addr][0]
    marker = f" @ {addr:08x}"
    out, on = [], False
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("// ") and " @ " in line:
                if on:
                    break
                on = line.rstrip().endswith(marker)
            if on:
                out.append(line.rstrip("\n"))
    return "\n".join(out).rstrip()


def parse_addr(s):
    return int(s, 16)


def summary(addr, show_pc, show_gc):
    print(f"=== 0x{addr:08X}")
    ours = our_symbols().get(addr)
    if ours:
        print(f"  ours     {ours['name']}  [{ours['status']}]  {ours['signature']}")
        if ours["notes"]:
            print(f"           {ours['notes'][:160]}")
    else:
        print("  ours     (not in docs/symbols.csv)")
    pc = dump_index("pc").get(addr)
    print(f"  pc dump  {pc[1]}  ({pc[0]})" if pc else "  pc dump  (not a function start in the PC export)")
    con = db()
    best_gc = None
    if con:
        rows = con.execute(
            "SELECT m.gc_addr, g.full_name, g.signature, g.src_file, m.method, m.confidence, m.note "
            "FROM matches m LEFT JOIN gc_functions g ON g.addr = m.gc_addr WHERE m.pc_addr = ? "
            "ORDER BY (m.method = 'manual') DESC, m.confidence DESC", (addr,)).fetchall()
        for gca, name, sig, src, method, conf, note in rows[:4]:
            flag = "" if method == "manual" or (conf or 0) >= 0.8 else "  <- low confidence, check"
            print(f"  GC match {name}  @ {gca:08X}  {method} {conf:.2f}{flag}")
            print(f"           {sig}  [{src or '?'}]" + (f"  note: {note}" if note else ""))
            if best_gc is None:
                best_gc = gca
        if not rows:
            print("  GC match (none in knowledge.db)")
        for k, v, s in con.execute(
                "SELECT key, value, source FROM facts WHERE subject_kind = 'pc_func' AND subject = ?", (f"{addr:X}",)):
            print(f"  fact     {k}: {v[:140]}  [{s}]")
        for status, file, note in con.execute(
                "SELECT status, file, note FROM rewrite WHERE pc_addr = ?", (addr,)):
            print(f"  gcpc     {status}  {file or ''}" + (f"  {note[:100]}" if note else ""))
    if show_pc:
        code = dump_code("pc", addr)
        print("\n" + (code if code else "  (no PC decompilation for this address)"))
    if show_gc:
        if best_gc is None:
            print("\n  (no GameCube match to show)")
        else:
            code = dump_code("gc", best_gc)
            print("\n" + (code if code else f"  (GC {best_gc:08X} not in the GC export)"))
    print()


def find(text):
    t = text.lower()
    hits = []
    for a, r in our_symbols().items():
        if t in r["name"].lower():
            hits.append(("ours", a, r["name"]))
    for which in ("pc", "gc"):
        for a, (_, name) in dump_index(which).items():
            if t in name.lower():
                hits.append((which, a, name))
    for src, a, name in hits[:60]:
        print(f"{src:4s} {a:08X}  {name}")
    if len(hits) > 60:
        print(f"... {len(hits) - 60} more")


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[0] == "find":
        find(" ".join(argv[1:]))
        return 0
    if argv[0] == "gc":
        for a in argv[1:]:
            code = dump_code("gc", parse_addr(a))
            print(code if code else f"(no GameCube function at {a})")
        return 0
    flags = {a for a in argv if a.startswith("--")}
    for a in argv:
        if not a.startswith("--"):
            summary(parse_addr(a), "--pc" in flags, "--gc" in flags)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
