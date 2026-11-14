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
TOTAL_FUNCTIONS = 23278  # functions in SPEED2.EXE (NFSU2Recomp's count, see CLAUDE.md)

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

# Planned systems in order (CLAUDE.md M8+). Folder names under game/systems map onto these.
SYSTEMS = [
    ("Leaf utilities", ["bstring", "bmath", "time"], "Strings, maths, hashing, RNG"),
    ("File & chunk loading", ["bchunk"], "Bundles, chunks, decompression"),
    ("Frontend / menus", ["frontend"], ""),
    ("Camera", ["camera"], ""),
    ("Car physics / handling", ["car", "physics"], ""),
    ("World streaming", ["world"], ""),
    ("AI racers / traffic", ["ai"], ""),
    ("Audio", ["audio"], ""),
    ("Rendering", ["render"], "Last: the future port boundary"),
]
FOLDER_LABEL = {"bstring": "Strings (bString)", "bmath": "Maths (bMath)", "time": "Frame time",
                "bchunk": "Chunks & JDLZ"}

STATUS_ORDER = {"verified": 0, "rewritten": 1, "named": 2, "unknown": 3}


def load_symbols():
    rows = []
    with open(ROOT / "docs" / "symbols.csv", newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def rewrite_folders():
    """function name -> game/systems folder, from NFSU2_REWRITE[_N[_EAX]] registrations."""
    out = {}
    for p in (ROOT / "game" / "systems").rglob("*.cpp"):
        folder = p.parent.name if p.parent.name != "systems" else "misc"
        for m in re.finditer(r"NFSU2_REWRITE(?:_N(?:_EAX)?)?\(\s*0x[0-9A-Fa-f]+\s*,\s*(\w+)", p.read_text()):
            out[m.group(1)] = folder
    return out


def bar_svg(x, y, w, h, frac, fg, bg="#1f2937"):
    fw = max(0.0, min(1.0, frac)) * w
    minw = 3 if frac > 0 else 0
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{h/2}" fill="{bg}"/>'
            f'<rect x="{x}" y="{y}" width="{max(fw, minw):.1f}" height="{h}" rx="{h/2}" fill="{fg}"/>')


def make_svg(verified, rewritten, per_system, milestones_done):
    W = 760
    rows = len(per_system)
    H = 250 + rows * 34
    pct = 100.0 * verified / TOTAL_FUNCTIONS
    t = []
    t.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
             f'font-family="Segoe UI, Helvetica, Arial, sans-serif">')
    t.append(f'<rect width="{W}" height="{H}" rx="14" fill="#0b1220"/>')
    t.append(f'<text x="28" y="44" font-size="22" font-weight="700" fill="#f9fafb">NFSU2 rewrite progress</text>')
    t.append(f'<text x="{W-28}" y="44" font-size="13" fill="#9ca3af" text-anchor="end">'
             f'updated {datetime.date.today().isoformat()}</text>')
    # overall
    t.append(f'<text x="28" y="84" font-size="15" fill="#e5e7eb">Functions rewritten and verified</text>')
    t.append(f'<text x="{W-28}" y="84" font-size="15" font-weight="700" fill="#34d399" text-anchor="end">'
             f'{verified:,} / {TOTAL_FUNCTIONS:,} ({pct:.2f}%)</text>')
    t.append(bar_svg(28, 96, W - 56, 22, verified / TOTAL_FUNCTIONS, "#34d399"))
    # milestones
    md = milestones_done
    t.append(f'<text x="28" y="152" font-size="15" fill="#e5e7eb">Milestones (tooling M0–M7, then the rewrite)</text>')
    t.append(f'<text x="{W-28}" y="152" font-size="15" font-weight="700" fill="#60a5fa" text-anchor="end">'
             f'{md} / {len(MILESTONES)} done</text>')
    seg_w = (W - 56 - (len(MILESTONES) - 1) * 6) / len(MILESTONES)
    for i, (mid, _, st) in enumerate(MILESTONES):
        x = 28 + i * (seg_w + 6)
        col = {"done": "#60a5fa", "in progress": "#fbbf24"}.get(st, "#1f2937")
        t.append(f'<rect x="{x:.1f}" y="164" width="{seg_w:.1f}" height="22" rx="6" fill="{col}"/>')
        t.append(f'<text x="{x + seg_w/2:.1f}" y="180" font-size="12" font-weight="700" '
                 f'fill="{"#0b1220" if st != "todo" else "#6b7280"}" text-anchor="middle">{mid}</text>')
    # systems
    t.append(f'<text x="28" y="222" font-size="15" fill="#e5e7eb">Verified functions per system</text>')
    maxn = max([n for _, n, _ in per_system] + [1])
    y = 240
    for name, n, note in per_system:
        col = "#34d399" if n else "#374151"
        t.append(f'<text x="28" y="{y+14}" font-size="13" fill="{"#e5e7eb" if n else "#6b7280"}">{html.escape(name)}</text>')
        t.append(bar_svg(250, y + 3, W - 250 - 90, 14, n / maxn, col))
        t.append(f'<text x="{W-28}" y="{y+14}" font-size="13" font-weight="700" fill="{"#e5e7eb" if n else "#6b7280"}" '
                 f'text-anchor="end">{n if n else "—"}</text>')
        y += 34
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
        per_folder.setdefault(folders.get(r["name"], "misc"), []).append(r)
    per_system = []
    for sname, flds, note in SYSTEMS:
        n = sum(len(per_folder.get(f, [])) for f in flds)
        per_system.append((sname, n, note))
    milestones_done = sum(1 for m in MILESTONES if m[2] == "done")

    (ROOT / "docs" / "progress.svg").write_text(make_svg(len(verified), len(rewritten), per_system, milestones_done))

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
    for sname, n, note in per_system:
        st = "🟡 started" if n else "⬜ not started"
        md.append(f"| {sname}{' — ' + note if note else ''} | {n} | {st} |")
    md += ["", "## Done (verified)", ""]
    by_folder = sorted(per_folder.items(), key=lambda kv: kv[0])
    for fld, rows in by_folder:
        md += [f"### {FOLDER_LABEL.get(fld, fld)} — {len(rows)}", "", "| Address | Function | Signature |", "|---|---|---|"]
        for r in sorted(rows, key=lambda r: r["addr"]):
            sig = r["signature"].replace("|", "\\|")
            md.append(f"| `{r['addr']}` | **{r['name']}** | `{sig}` |")
        md.append("")
    md += ["## Not done yet", "", "Functions we have found and named but not rewritten yet (everything else in the "
           "exe is still unexplored).", "", "| Address | Name | Notes |", "|---|---|---|"]
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
