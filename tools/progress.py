#!/usr/bin/env python3
"""Generate the progress tracker: docs/progress.svg (card for the README) and PROGRESS.md (full lists).

Sources: docs/symbols.csv (status per function) and the rewrite sources in game/systems/<system>/ (which system a
rewrite belongs to). Run after every batch:  .venv/bin/python tools/progress.py
"""
from __future__ import annotations

import csv
import datetime
import html
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Game functions in SPEED2.EXE: Ghidra 12.2 functions below 0x75BBFA, where the Winsock import stubs and the MSVC
# runtime start (counted 2026-11-15 from our PC export; the GameCube build has 14,666).
TOTAL_FUNCTIONS = 14450

# Milestones from CLAUDE.md, with their state. Update when one completes.
MILESTONES = [
    ("M0", "Game runs under Wine", "done"),
    ("M1", "Probe + MCP bridge", "done"),
    ("M2", "Unattended launcher", "done"),
    ("M3", "Input injection", "done"),
    ("M4", "Boot straight to free roam", "done"),
    ("M5", "Telemetry + scripted driving", "done"),
    ("M6", "Rewrite DLL + hot reload", "done"),
    ("M7", "Diff harness", "done"),
    ("M8", "System-by-system rewrite", "in progress"),
]

# Planned systems in order (CLAUDE.md M8+). Each groups one or more folders under game/systems/.
SYSTEMS = [
    ("Core utilities", ["bstring", "bmath", "memory"], "strings, maths, memory allocator"),
    ("Files & data loading", ["file", "resource", "bchunk"], "file layer, resource files, chunk bundles"),
    ("Frontend / menus", ["frontend"], "menus, shops, career screens"),
    ("Camera", ["camera"], "chase / bumper / photo cameras"),
    ("Car physics & handling", ["car", "physics"], "engine, tyres, suspension, collisions"),
    ("World streaming", ["world"], "loading Bayview around the player"),
    ("AI racers & traffic", ["ai"], "opponents, traffic cars"),
    ("Audio", ["audio"], "engine sounds, music, effects"),
    ("Rendering", ["render"], "last: the future port boundary"),
]
FOLDER_LABEL = {"bstring": "Strings", "bmath": "Maths", "memory": "Memory",
                "bchunk": "Chunks", "resource": "Resource files", "file": "File layer"}

STATUS_ORDER = {"verified": 0, "rewritten": 1, "named": 2, "unknown": 3}

# colours
BG, PANEL, LINE = "#0d1117", "#161b22", "#30363d"
TXT, DIM, FAINT = "#e6edf3", "#8b949e", "#484f58"
GREEN, AMBER, BLUE = "#3fb950", "#d29922", "#58a6ff"


GAME_END = 0x75BBFA  # first non-game address (Winsock stubs, then the MSVC runtime)


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


def load_symbols():
    rows = []
    with open(ROOT / "docs" / "symbols.csv", newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def rewrite_folders():
    """address -> game/systems folder, from every rewrite registration (NFSU2_REWRITE*, Registrar)."""
    out = {}
    pat = re.compile(r'(?:NFSU2_REWRITE(?:_N(?:_EAX)?|_THIS)?|Registrar\s+\w+)\(\s*(0x[0-9A-Fa-f]+)')
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


def check(x, y, done, current=False):
    if done:
        return (f'<circle cx="{x}" cy="{y}" r="8" fill="{GREEN}"/>'
                f'<path d="M{x-4} {y} l3 3 l5 -6" stroke="{BG}" stroke-width="2" fill="none" stroke-linecap="round"/>')
    if current:
        return f'<circle cx="{x}" cy="{y}" r="8" fill="none" stroke="{AMBER}" stroke-width="2"/><circle cx="{x}" cy="{y}" r="3.5" fill="{AMBER}"/>'
    return f'<circle cx="{x}" cy="{y}" r="8" fill="none" stroke="{FAINT}" stroke-width="2"/>'


def pill(x, y, text, col):
    w = 9 + 6.6 * len(text)
    return (f'<rect x="{x - w:.1f}" y="{y - 13}" width="{w:.1f}" height="18" rx="9" fill="{col}" fill-opacity="0.15" '
            f'stroke="{col}" stroke-opacity="0.6"/><text x="{x - w/2:.1f}" y="{y}" font-size="11" font-weight="600" '
            f'fill="{col}" text-anchor="middle">{esc(text)}</text>')


def make_svg(verified, mapped, per_system, per_folder_counts):
    W, M = 840, 28
    goal = round(TOTAL_FUNCTIONS * 0.01)  # first goal: 1 % of the exe
    rows = len(per_system)
    H = 470 + rows * 46
    t = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif">',
         f'<rect width="{W}" height="{H}" rx="12" fill="{BG}" stroke="{LINE}"/>',
         f'<text x="{M}" y="42" font-size="21" font-weight="700" fill="{TXT}">Need for Speed: Underground 2 rewrite</text>',
         f'<text x="{W-M}" y="42" font-size="12" fill="{DIM}" text-anchor="end">updated {datetime.date.today().isoformat()}</text>',
         f'<text x="{M}" y="64" font-size="13" fill="{DIM}">Our own C++ replacing the game one function at a time, each proven identical to the original before it counts.</text>']

    # --- hero: big number + next goal
    y0 = 88
    t.append(f'<rect x="{M}" y="{y0}" width="{W-2*M}" height="118" rx="10" fill="{PANEL}" stroke="{LINE}"/>')
    t.append(f'<text x="{M+22}" y="{y0+62}" font-size="52" font-weight="800" fill="{GREEN}">{verified}</text>')
    nx = M + 22 + 31 * len(str(verified)) + 14
    t.append(f'<text x="{nx}" y="{y0+44}" font-size="16" font-weight="600" fill="{TXT}">functions rewritten &amp; verified</text>')
    t.append(f'<text x="{nx}" y="{y0+64}" font-size="13" fill="{DIM}">running in the game right now, switchable back to the original</text>')
    gx, gw = M + 22, W - 2 * M - 44
    t.append(f'<text x="{gx}" y="{y0+92}" font-size="12" fill="{TXT}">Next goal: 1% of the game ({goal} functions)</text>')
    t.append(f'<text x="{gx+gw}" y="{y0+92}" font-size="12" font-weight="700" fill="{GREEN}" text-anchor="end">'
             f'{verified} / {goal} · {100*verified/goal:.0f}%</text>')
    t.append(bar(gx, y0 + 100, gw, 8, [(min(1, verified / goal), GREEN)]))

    # --- whole game
    y1 = y0 + 146
    pct_v = 100.0 * verified / TOTAL_FUNCTIONS
    t.append(f'<text x="{M}" y="{y1}" font-size="14" font-weight="600" fill="{TXT}">Whole game</text>')
    t.append(f'<text x="{W-M}" y="{y1}" font-size="12" fill="{DIM}" text-anchor="end">~{TOTAL_FUNCTIONS:,} game functions in SPEED2.EXE (runtime libraries not counted)</text>')
    t.append(bar(M, y1 + 10, W - 2 * M, 14, [(verified / TOTAL_FUNCTIONS, GREEN), (mapped / TOTAL_FUNCTIONS, AMBER)]))
    ly = y1 + 44
    t.append(f'<rect x="{M}" y="{ly-10}" width="10" height="10" rx="2" fill="{GREEN}"/>'
             f'<text x="{M+16}" y="{ly}" font-size="12" fill="{TXT}">verified {verified} ({pct_v:.2f}%)</text>')
    t.append(f'<rect x="{M+190}" y="{ly-10}" width="10" height="10" rx="2" fill="{AMBER}"/>'
             f'<text x="{M+206}" y="{ly}" font-size="12" fill="{TXT}">EA name known, not rewritten yet: {mapped:,}</text>')
    t.append(f'<rect x="{M+470}" y="{ly-10}" width="10" height="10" rx="2" fill="{LINE}"/>'
             f'<text x="{M+486}" y="{ly}" font-size="12" fill="{DIM}">no name yet</text>')

    # --- milestones (3 columns)
    y2 = ly + 40
    done = sum(1 for m in MILESTONES if m[2] == "done")
    t.append(f'<text x="{M}" y="{y2}" font-size="14" font-weight="600" fill="{TXT}">Milestones</text>')
    t.append(f'<text x="{W-M}" y="{y2}" font-size="12" fill="{DIM}" text-anchor="end">{done} of {len(MILESTONES)} done · tools first (M0–M7), then the rewrite itself</text>')
    colw = (W - 2 * M) / 3
    for i, (mid, name, st) in enumerate(MILESTONES):
        cx = M + (i % 3) * colw
        cy = y2 + 28 + (i // 3) * 28
        t.append(check(cx + 9, cy - 4, st == "done", st == "in progress"))
        col = TXT if st != "todo" else DIM
        t.append(f'<text x="{cx+26}" y="{cy}" font-size="12.5" fill="{col}"><tspan font-weight="700">{mid}</tspan> {esc(name)}</text>')

    # --- systems
    y3 = y2 + 28 + ((len(MILESTONES) + 2) // 3) * 28 + 18
    t.append(f'<text x="{M}" y="{y3}" font-size="14" font-weight="600" fill="{TXT}">Systems</text>')
    t.append(f'<text x="{W-M}" y="{y3}" font-size="12" fill="{DIM}" text-anchor="end">verified functions per system, in planned order</text>')
    y = y3 + 14
    maxn = max([n for _, n, _, _ in per_system] + [1])
    for sname, n, note, flds in per_system:
        t.append(f'<rect x="{M}" y="{y}" width="{W-2*M}" height="38" rx="8" fill="{PANEL}" stroke="{LINE}"/>')
        started = n > 0
        t.append(f'<text x="{M+14}" y="{y+17}" font-size="13" font-weight="600" fill="{TXT if started else DIM}">{esc(sname)}</text>')
        parts = " · ".join(f"{FOLDER_LABEL.get(f, f)} {per_folder_counts.get(f, 0)}" for f in flds if per_folder_counts.get(f, 0))
        t.append(f'<text x="{M+14}" y="{y+31}" font-size="11" fill="{DIM}">{esc(parts if parts else note)}</text>')
        bx, bw = M + 300, W - 2 * M - 300 - 150
        t.append(bar(bx, y + 15, bw, 8, [(n / maxn, GREEN if started else LINE)]))
        t.append(f'<text x="{bx+bw+34}" y="{y+23}" font-size="14" font-weight="700" fill="{TXT if started else FAINT}" text-anchor="end">{n if started else "–"}</text>')
        t.append(pill(W - M - 12, y + 24, "in progress" if started else "not started", AMBER if started else FAINT))
        y += 46
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
        per_folder.setdefault(folders.get(int(r["addr"], 16), "misc"), []).append(r)
    counts = {f: len(v) for f, v in per_folder.items()}
    per_system = []
    for sname, flds, note in SYSTEMS:
        per_system.append((sname, sum(counts.get(f, 0) for f in flds), note, flds))
    unassigned = [r["name"] for r in verified if folders.get(int(r["addr"], 16)) is None]
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
        "## Milestones",
        "",
        "| | Milestone | Status |",
        "|---|---|---|",
    ]
    icon = {"done": "✅", "in progress": "🟡", "todo": "⬜"}
    for mid, name, st in MILESTONES:
        md.append(f"| {mid} | {name} | {icon.get(st, '⬜')} {st} |")
    md += ["", "## Systems", "", "| System | Verified | Status |", "|---|---:|---|"]
    for sname, n, note, flds in per_system:
        st = "🟡 in progress" if n else "⬜ not started"
        parts = ", ".join(f"{FOLDER_LABEL.get(f, f)} {counts.get(f, 0)}" for f in flds if counts.get(f, 0))
        md.append(f"| **{sname}** — {parts if parts else note} | {n} | {st} |")
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
