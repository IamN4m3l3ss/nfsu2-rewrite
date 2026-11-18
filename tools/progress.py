#!/usr/bin/env python3
"""Generate the progress tracker: docs/progress.svg (card for the README) and PROGRESS.md (full lists).

Sources: docs/symbols.csv (status per function) and the rewrite sources in game/systems/<system>/ (which system a
rewrite belongs to). Run after every batch:  .venv/bin/python tools/progress.py
"""
from __future__ import annotations

import csv
import datetime
import html
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Game functions in SPEED2.EXE: Ghidra 12.2 functions below 0x75BBFA, where the Winsock import stubs and the MSVC
# runtime start (counted 2026-11-15 from our PC export; the GameCube build has 14,666).
TOTAL_FUNCTIONS = 14450

# Milestones from CLAUDE.md, with their state. Update when one completes.

# Planned systems in order (CLAUDE.md M8+): (name, GameCube source files that belong to it (regex), description).
# Every PC game function counts in the system of the GameCube source file it matches (knowledge.db matches from the
# RE kit); size and verified count are both PC functions, so the percentages are exact for the mapped part. PC
# functions with no GameCube match are shown as their own row until they get mapped. The mapping is cached in
# docs/pc_systems.json so the tracker also works without the RE kit.
SYSTEMS = [
    ("Core utilities", [r"zBWare"], "strings, maths, memory, lists, slot pools"),
    ("Files & data loading", [r"filesys", r"syncfile", r"FSasync", r"zMisc", r"hlafile", r"memcard", r"dlopen"],
     "file system, resource files, chunks, misc helpers"),
    ("Frontend / menus", [r"zFe2?\.cpp"], "menus, shops, career screens"),
    ("Camera", [r"zCamera"], "chase / bumper / photo cameras"),
    ("Car physics & handling", [r"zPhysics"], "engine, tyres, suspension, collisions"),
    ("World streaming", [r"zWorld", r"zTrack"], "loading Bayview around the player"),
    ("AI, traffic & race rules", [r"zAI", r"zGameModes"], "opponents, traffic cars, game modes"),
    ("Animation", [r"zAnim", r"Blender"], "skeletal animation, blenders"),
    ("Audio & video", [r"zEAXSound", r"\.c$", r"mad", r"codec", r"mtdec"], "engine sounds, music, speech, movies"),
    ("Rendering", [r"zPlatform"], "last: the future port boundary"),
]
UNMAPPED = "Not mapped to a system yet"
FOLDER_LABEL = {"bstring": "Strings", "bmath": "Maths", "memory": "Memory", "bware": "Lists & pools", "frontend": "Menu engine (FEng)", "camera": "Camera",
                "bchunk": "Chunks", "resource": "Resource files", "file": "File layer"}

STATUS_ORDER = {"verified": 0, "rewritten": 1, "named": 2, "unknown": 3}

# colours
BG, PANEL, LINE = "#0d1117", "#161b22", "#30363d"
TXT, DIM, FAINT = "#e6edf3", "#8b949e", "#484f58"
GREEN, AMBER, BLUE = "#3fb950", "#d29922", "#58a6ff"


GAME_END = 0x75BBFA  # first non-game address (Winsock stubs, then the MSVC runtime)


def pc_systems():
    """{system: {gc source file: [pc addresses]}} - computed from the RE kit when present, else the cached json."""
    cache = ROOT / "docs" / "pc_systems.json"
    try:
        sys.path.insert(0, str(ROOT / "tools" / "re"))
        import sqlite3
        import kb
        if not kb.DB:
            raise RuntimeError("no RE kit")
        db = sqlite3.connect(f"file:{kb.DB}?mode=ro", uri=True)
        out = {name: {} for name, _, _ in SYSTEMS}
        seen = set()  # a PC function matched to several GameCube functions counts once (first match)
        for pa, sf in db.execute("select m.pc_addr, g.src_file from matches m join gc_functions g on g.addr = m.gc_addr "
                                 "order by m.rowid"):
            if not sf or pa >= GAME_END or pa in seen:
                continue
            seen.add(pa)
            f = re.sub(r".*[/\\\\]", "", sf)
            for name, pats, _ in SYSTEMS:
                if any(re.search(p, f) for p in pats):
                    out[name].setdefault(f, set()).add(pa)
                    break
        out = {n: {f: sorted(f"{a:06X}" for a in v) for f, v in d.items()} for n, d in out.items()}
        cache.write_text(json.dumps(out, indent=0, sort_keys=True))
        return out
    except Exception:  # noqa: BLE001
        return json.loads(cache.read_text()) if cache.exists() else {name: {} for name, _, _ in SYSTEMS}


def ea_named():
    """Game functions with an EA name in the PC decompilation (RE kit, local only): set of addresses, or None."""
    env = {}
    for line in (ROOT / ".env.local").read_text().splitlines() if (ROOT / ".env.local").exists() else []:
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"')
    tsv = Path(env.get("PC_DECOMP", "/nonexistent")) / "functions.tsv"
    if not tsv.exists():
        return None
    out = set()
    with open(tsv) as f:
        next(f)
        for line in f:
            a, _, name = line.rstrip("\n").split("\t")
            addr = int(a, 16)
            if addr < GAME_END and not re.match(r"^(thunk_)?FUN_[0-9a-f]+$", name):
                out.add(addr)
    return out


def gcpc_folder(row):
    """nfsu2-gcpc rewrites registered at run time (frontend/gcpc_hooks.cpp): system from the 'gcpc:<folder>' note."""
    n = row.get("notes") or ""
    return n[5:n.index(";")] if n.startswith("gcpc:") and ";" in n else None


def load_symbols():
    rows = []
    with open(ROOT / "docs" / "symbols.csv", newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def rewrite_folders():
    """address -> game/systems folder, from every rewrite registration (NFSU2_REWRITE*, ACC_* accessors, Registrar)."""
    out = {}
    pat = re.compile(r'(?:NFSU2_REWRITE(?:_N(?:_EAX)?|_THIS|_STD)?|ACC_\w+|LEAF_\w+|Registrar\s+\w+)\(\s*(0x[0-9A-Fa-f]+)')
    for p in (ROOT / "game" / "systems").rglob("*.cpp"):
        folder = p.parent.name if p.parent.name != "systems" else "misc"
        for m in pat.finditer(p.read_text()):
            out[int(m.group(1), 16)] = folder
    return out


def esc(s):
    return html.escape(s)


def bar(x, y, w, h, segs, bg=LINE):
    """segs: [(fraction, colour)] stacked left to right; tiny non-zero segments get a visible sliver."""
    t = [f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{h/2}" fill="{bg}"/>']
    cx = x
    for frac, col in segs:
        sw = max(frac * w, 4 if frac > 0 else 0)
        if sw <= 0:
            continue
        t.append(f'<rect x="{cx:.1f}" y="{y}" width="{sw:.1f}" height="{h}" rx="{min(h/2, sw/2):.1f}" fill="{col}"/>')
        cx += sw
    return "".join(t)


def pill(x, y, text, col):
    w = 9 + 6.6 * len(text)
    return (f'<rect x="{x - w:.1f}" y="{y - 13}" width="{w:.1f}" height="18" rx="9" fill="{col}" fill-opacity="0.15" '
            f'stroke="{col}" stroke-opacity="0.6"/><text x="{x - w/2:.1f}" y="{y}" font-size="11" font-weight="600" '
            f'fill="{col}" text-anchor="middle">{esc(text)}</text>')


def make_svg(verified, mapped, per_system, _counts=None):
    """The tracker card: hero (count, share, next goal), whole-game bar, systems as a two-column grid."""
    W, M, G = 880, 28, 14
    pct = 100.0 * verified / TOTAL_FUNCTIONS
    goal_pct = next((g for g in (1, 2, 5, 10, 25, 50, 100) if verified < round(TOTAL_FUNCTIONS * g / 100)), 100)
    goal = round(TOTAL_FUNCTIONS * goal_pct / 100)
    systems = [r for r in per_system if r[0] != UNMAPPED]
    unmapped = next((r for r in per_system if r[0] == UNMAPPED), None)
    rows = (len(systems) + 1) // 2
    CH = 74                                     # system card height
    y_sys = 320
    H = y_sys + 34 + rows * (CH + G) + (70 if unmapped else 0) + 24
    t = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif">',
         '<defs><linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="#2ea043"/>'
         f'<stop offset="1" stop-color="{GREEN}"/></linearGradient></defs>',
         f'<rect width="{W}" height="{H}" rx="14" fill="{BG}" stroke="{LINE}"/>']
    # header
    t.append(f'<text x="{M}" y="44" font-size="22" font-weight="800" fill="{TXT}">NFS Underground 2 — rewrite</text>')
    t.append(f'<text x="{M}" y="66" font-size="12.5" fill="{DIM}">Our own C++ replacing the game function by function; '
             f'each one proven identical to the original before it counts.</text>')
    t.append(f'<text x="{W-M}" y="44" font-size="12" fill="{DIM}" text-anchor="end">updated {datetime.date.today().isoformat()}</text>')

    # hero panel
    hy, hh = 86, 120
    t.append(f'<rect x="{M}" y="{hy}" width="{W-2*M}" height="{hh}" rx="12" fill="{PANEL}" stroke="{LINE}"/>')
    t.append(f'<text x="{M+24}" y="{hy+68}" font-size="56" font-weight="800" fill="url(#g)">{verified:,}</text>')
    t.append(f'<text x="{M+26}" y="{hy+96}" font-size="13.5" fill="{TXT}">functions verified · '
             f'<tspan font-weight="700" fill="{GREEN}">{pct:.1f}%</tspan> of the game</text>')
    # goal ring
    cx, cy, r = W - M - 78, hy + hh / 2, 40
    frac = min(1.0, verified / goal)
    circ = 2 * 3.14159265 * r
    t.append(f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="{LINE}" stroke-width="9"/>')
    t.append(f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="{GREEN}" stroke-width="9" stroke-linecap="round" '
             f'stroke-dasharray="{circ*frac:.1f} {circ:.1f}" transform="rotate(-90 {cx} {cy})"/>')
    t.append(f'<text x="{cx}" y="{cy+6}" font-size="17" font-weight="800" fill="{TXT}" text-anchor="middle">{100*frac:.0f}%</text>')
    t.append(f'<text x="{cx-r-18}" y="{cy-8}" font-size="12" fill="{DIM}" text-anchor="end">next goal</text>')
    t.append(f'<text x="{cx-r-18}" y="{cy+10}" font-size="13" font-weight="700" fill="{TXT}" text-anchor="end">'
             f'{goal_pct}% · {goal:,}</text>')
    t.append(f'<text x="{cx-r-18}" y="{cy+27}" font-size="11.5" fill="{DIM}" text-anchor="end">{max(goal-verified, 0):,} to go</text>')

    # whole game bar + three figures
    wy = hy + hh + 34
    t.append(f'<text x="{M}" y="{wy}" font-size="13" font-weight="700" fill="{TXT}">Whole game</text>')
    t.append(f'<text x="{W-M}" y="{wy}" font-size="11.5" fill="{DIM}" text-anchor="end">{TOTAL_FUNCTIONS:,} game functions in '
             f'SPEED2.EXE (runtime libraries not counted)</text>')
    t.append(bar(M, wy + 12, W - 2 * M, 12, [(verified / TOTAL_FUNCTIONS, GREEN), (mapped / TOTAL_FUNCTIONS, AMBER)]))
    unnamed = max(TOTAL_FUNCTIONS - verified - mapped, 0)
    fx = M
    for col, num, label in ((GREEN, verified, "verified"), (AMBER, mapped, "EA name known, still to do"),
                            (LINE, unnamed, "no name yet")):
        t.append(f'<rect x="{fx}" y="{wy+40}" width="10" height="10" rx="3" fill="{col}"/>'
                 f'<text x="{fx+16}" y="{wy+49}" font-size="12" fill="{TXT}"><tspan font-weight="700">{num:,}</tspan> '
                 f'<tspan fill="{DIM}">{label}</tspan></text>')
        fx += 250

    # systems grid
    t.append(f'<text x="{M}" y="{y_sys}" font-size="13" font-weight="700" fill="{TXT}">Systems</text>')
    t.append(f'<text x="{W-M}" y="{y_sys}" font-size="11.5" fill="{DIM}" text-anchor="end">verified / PC functions, '
             f'in planned order</text>')
    cw = (W - 2 * M - G) / 2
    for k, (sname, n, parts, total) in enumerate(systems):
        x = M + (k % 2) * (cw + G)
        y = y_sys + 16 + (k // 2) * (CH + G)
        fr = n / total if total else 0
        done = total and n >= total
        col = GREEN if (done or fr >= 0.5) else (BLUE if n else FAINT)
        t.append(f'<rect x="{x:.1f}" y="{y}" width="{cw:.1f}" height="{CH}" rx="10" fill="{PANEL}" stroke="{LINE}"/>')
        t.append(f'<text x="{x+14:.1f}" y="{y+24}" font-size="13.5" font-weight="700" fill="{TXT if n else DIM}">'
                 f'<tspan fill="{FAINT}">{k+1:02d}</tspan>  {esc(sname)}</text>')
        t.append(f'<text x="{x+cw-14:.1f}" y="{y+24}" font-size="15" font-weight="800" fill="{col if n else FAINT}" '
                 f'text-anchor="end">{100*fr:.0f}%</text>')
        t.append(bar(x + 14, y + 34, cw - 28, 7, [(fr, col)]))
        short = parts if len(parts) <= 46 else parts[:parts.rfind(" · ", 0, 46)] + " …"
        t.append(f'<text x="{x+14:.1f}" y="{y+61}" font-size="11" fill="{DIM}">{esc(short)}</text>')
        t.append(f'<text x="{x+cw-14:.1f}" y="{y+61}" font-size="11.5" font-weight="600" fill="{TXT if n else DIM}" '
                 f'text-anchor="end">{n:,} / {total:,}</text>')
    if unmapped:
        sname, n, parts, total = unmapped
        y = y_sys + 16 + rows * (CH + G)
        t.append(f'<rect x="{M}" y="{y}" width="{W-2*M}" height="56" rx="10" fill="{BG}" stroke="{LINE}" stroke-dasharray="4 4"/>')
        t.append(f'<text x="{M+14}" y="{y+22}" font-size="12.5" font-weight="700" fill="{DIM}">{esc(sname)}</text>')
        t.append(f'<text x="{W-M-14}" y="{y+22}" font-size="11.5" fill="{DIM}" text-anchor="end">{n:,} verified / {total:,} '
                 f'· no GameCube match yet, still counted in the whole-game total</text>')
        t.append(bar(M + 14, y + 34, W - 2 * M - 28, 6, [(n / total if total else 0, FAINT)]))
    t.append("</svg>")
    return "\n".join(t)


def main():
    syms = load_symbols()
    folders = rewrite_folders()
    verified = [r for r in syms if r["status"] == "verified"]
    rewritten = [r for r in syms if r["status"] == "rewritten"]
    named = [r for r in syms if r["status"] == "named"]

    per_folder = {}
    for r in verified:
        per_folder.setdefault(folders.get(int(r["addr"], 16)) or gcpc_folder(r) or "misc", []).append(r)
    counts = {f: len(v) for f, v in per_folder.items()}
    per_system = []
    vset = {int(r["addr"], 16) for r in verified}
    mapping = pc_systems()
    mapped = set()
    for sname, _pats, note in SYSTEMS:
        files = mapping.get(sname, {})
        addrs = {int(a, 16) for v in files.values() for a in v}
        mapped |= addrs
        n = len(addrs & vset)
        parts = " · ".join(f"{f.rsplit('.', 1)[0]} {len({int(a, 16) for a in v} & vset)}/{len(v):,}"
                           for f, v in sorted(files.items(), key=lambda kv: -len(kv[1]))[:4])
        per_system.append((sname, n, parts or note, len(addrs)))
    per_system.append((UNMAPPED, len(vset - mapped), "PC functions with no GameCube match (yet): counted in the total above",
                       max(TOTAL_FUNCTIONS - len(mapped), 1)))
    unassigned = [r["name"] for r in verified if folders.get(int(r["addr"], 16)) is None and not gcpc_folder(r)]
    if unassigned:
        print("warning: verified but no rewrite registration found:", ", ".join(unassigned))

    # amber: functions whose EA name is known (decompilation), not verified yet; without the RE kit: our own names
    known = ea_named()
    verified_addrs = {int(r["addr"], 16) for r in verified}
    if known is not None:
        known |= {int(r["addr"], 16) for r in rewritten + named if int(r["addr"], 16) < GAME_END}
        named_not_done = len(known - verified_addrs)
    else:
        named_not_done = len(rewritten) + len(named)
    (ROOT / "docs" / "progress.svg").write_text(make_svg(len(verified), named_not_done, per_system, counts))
    # cache-buster: GitHub/browsers cache README images, so the link changes whenever the numbers do
    readme = ROOT / "README.md"
    tag = f"{len(verified)}-{datetime.date.today().isoformat()}"
    text = readme.read_text()
    new = re.sub(r"!\[progress\]\(docs/progress\.svg(\?v=[^)]*)?\)", f"![progress](docs/progress.svg?v={tag})", text)
    if new != text:
        readme.write_text(new)

    pct = 100.0 * len(verified) / TOTAL_FUNCTIONS
    blocks = round(pct / 5)  # 20-char text bar (every block = 5 %), at least one sliver when > 0
    bar = "█" * blocks + ("▏" if blocks == 0 and verified else "") + "░" * (20 - blocks)
    md = [
        "# Progress",
        "",
        f"_Generated by `tools/progress.py` on {datetime.date.today().isoformat()} from `docs/symbols.csv`._",
        "",
        "![progress](docs/progress.svg)",
        "",
        f"**{len(verified):,} of ~{TOTAL_FUNCTIONS:,} functions rewritten and verified ({pct:.2f}%)**  ",
        f"`{bar}`",
        "",
        "A function counts as **verified** only when our C++ replaces the original inside the running game and the "
        "diff harness proves it identical: same results (floats bit-for-bit) and same register behaviour, on "
        "thousands of random and real inputs.",
        "",
    ]
    md += ["", "## Systems", "", "Each PC game function counts in the system of the GameCube source file it matches "
           "(EA names via nfsu2-gcpc's knowledge base); verified and size are both PC functions. Functions without a "
           "GameCube match yet have their own row.", "", "| System | Verified | PC functions | Done | Status |",
           "|---|---:|---:|---:|---|"]
    for sname, n, parts, total in per_system:
        st = ("🔍 mapping" if sname == UNMAPPED else "✅ done" if n >= total else ("🟡 in progress" if n else "⬜ not started"))
        md.append(f"| **{sname}** — {parts} | {n} | {total:,} | {100*n/total:.0f}% | {st} |")
    md += ["", "## Done (verified)", ""]
    by_folder = sorted(per_folder.items(), key=lambda kv: kv[0])
    for fld, rows in by_folder:
        md += [f"### {FOLDER_LABEL.get(fld, fld)} — {len(rows)}", "", "| Address | Function | Signature |", "|---|---|---|"]
        for r in sorted(rows, key=lambda r: r["addr"]):
            sig = r["signature"].replace("|", "\\|")
            md.append(f"| `{r['addr']}` | **{r['name']}** | `{sig}` |")
        md.append("")
    known_txt = (f"About {named_not_done:,} more game functions have a known EA name (from the GameCube build, via "
                 "nfsu2-gcpc) but aren't rewritten yet. " if known is not None else "")
    md += ["## Not done yet", "", known_txt + "Below: the ones we have studied ourselves (notes, sometimes partly "
           "rewritten).", "", "| Address | Name | Notes |", "|---|---|---|"]
    for r in sorted(rewritten + named, key=lambda r: (STATUS_ORDER[r["status"]], r["addr"])):
        notes = (r["notes"] or "").replace("|", "\\|")
        if len(notes) > 110:
            notes = notes[:107] + "…"
        tag = " _(rewritten, not verified)_" if r["status"] == "rewritten" else ""
        md.append(f"| `{r['addr']}` | {r['name']}{tag} | {notes} |")
    md.append("")
    (ROOT / "PROGRESS.md").write_text("\n".join(md))
    print(f"verified {len(verified)} / {TOTAL_FUNCTIONS} ({pct:.2f}%), rewritten {len(rewritten)}, named {len(named)}; "
          f"wrote docs/progress.svg and PROGRESS.md")


if __name__ == "__main__":
    main()
