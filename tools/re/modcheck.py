#!/usr/bin/env python3
"""Which rewrites must stay off because of the user's mods (needs the running game + probe).

The mods (WidescreenFix, HD UI support, Extra Options) patch code inside ~80 game functions. A rewrite may not replace
such a function, nor one that reaches patched code through its calls: rewritten code calls our own versions directly
and would skip the mod (garbled menu text, 4:3 HUD - 2026-11-15).

  modcheck.py patched                 list the functions the mods patched (our own hook jumps excluded)
  modcheck.py exclude ADDR_FILE       read candidate addresses (hex, one per line or comma separated), print the ones
                                      to exclude: patched themselves, reading a constant the mods patched in .rdata
                                      (e.g. WidescreenFix's 640/320), or reaching (transitively through other
                                      candidates; calls AND tail jumps) a patched function. Virtual calls are not
                                      followed: still screenshot the HUD/menus after enabling a batch.
"""
import bisect
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "probe"))
import kb  # noqa: E402
import probe_client as pc  # noqa: E402
import xrefs  # noqa: E402

GAME_END = 0x75BBFA


def function_starts():
    return sorted(kb.dump_index("pc"))


def section(pe, name):
    return [s for s in pe.sections if s.Name.rstrip(b"\0") == name][0]


def mem_bytes(start, size):
    return b"".join(bytes.fromhex(pc.call("read_mem", {"addr": hex(start + o), "size": min(0x10000, size - o)}, timeout=60)["hex"])
                    for o in range(0, size, 0x10000))


def patched_constants(pe):
    """.rdata dwords the mods changed (the import table, filled in by the loader, excluded)."""
    rd = section(pe, b".rdata")
    start, size = 0x400000 + rd.VirtualAddress, min(rd.Misc_VirtualSize, rd.SizeOfRawData)
    disk, mem = pe.get_data(rd.VirtualAddress, size), mem_bytes(start, size)
    # import slots: filled in by the Windows loader, always differ from disk. This exe has no IAT data directory, so
    # take every slot from the import descriptors.
    slots = set()
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for imp in d.imports:
            slots.add(imp.address)
    out = set()
    for i in range(0, size - 3, 4):
        if disk[i:i + 4] != mem[i:i + 4] and start + i not in slots:
            out.add(start + i)
    return out


def constant_readers(pe, consts):
    """Functions whose code contains one of the constants' addresses (as a displacement/immediate)."""
    text = section(pe, b".text")
    code = pe.get_data(text.VirtualAddress, text.SizeOfRawData)
    base = 0x400000 + text.VirtualAddress
    idx = function_starts()
    out = {}
    for c in consts:
        pat, i = c.to_bytes(4, "little"), 0
        while (i := code.find(pat, i)) != -1:
            k = bisect.bisect_right(idx, base + i) - 1
            if k >= 0:
                out.setdefault(idx[k], []).append((base + i, 4))
            i += 1
    return out


def patched_functions():
    pe = xrefs.pe()
    text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
    start, size = 0x400000 + text.VirtualAddress, text.Misc_VirtualSize
    disk = pe.get_data(text.VirtualAddress, size)
    mem = b"".join(bytes.fromhex(pc.call("read_mem", {"addr": hex(start + o), "size": min(0x10000, size - o)}, timeout=60)["hex"])
                   for o in range(0, size, 0x10000))
    ours = {int(x["addr"], 16) for x in pc.call("rewrite_list")["rewrites"] if x["enabled"]}
    idx = function_starts()
    out = {}
    i = 0
    while i < size:
        if disk[i] != mem[i]:
            j = i
            while j < size and (disk[j] != mem[j] or (j + 1 < size and disk[j + 1] != mem[j + 1])):
                j += 1
            a, n = start + i, j - i
            if not (a in ours and n <= 8):  # our own MinHook jump
                k = bisect.bisect_right(idx, a) - 1
                out.setdefault(idx[k] if k >= 0 else a, []).append((a, n))
            i = j
        else:
            i += 1
    for f, spots in constant_readers(pe, patched_constants(pe)).items():
        out.setdefault(f, []).extend(spots)
    return out


def callees(pe, nxt, a):
    size = min(nxt.get(a, a + 64) - a, 8192)
    b = pe.get_data(a - 0x400000, size)
    out = set()
    end = a + size
    for i in range(len(b) - 4):
        t = None
        if b[i] in (0xE8, 0xE9):  # call / jmp rel32
            t = a + i + 5 + struct.unpack("<i", b[i + 1:i + 5])[0]
        elif b[i] == 0x0F and 0x80 <= b[i + 1] <= 0x8F and i + 6 <= len(b):  # jcc rel32
            t = a + i + 6 + struct.unpack("<i", b[i + 2:i + 6])[0]
        if t is None or not 0x401000 <= t < GAME_END:
            continue
        if b[i] == 0xE8 or not a <= t < end:  # jumps count only when they leave the function (tail calls)
            out.add(t)
    return out


def exclusions(candidates, patched):
    pe = xrefs.pe()
    idx = function_starts()
    nxt = {a: idx[i + 1] for i, a in enumerate(idx[:-1])}
    cg = {a: callees(pe, nxt, a) for a in candidates}
    bad = {a for a in candidates if a in patched or cg[a] & patched}
    changed = True
    while changed:
        changed = False
        for a in candidates:
            if a not in bad and cg[a] & bad:
                bad.add(a)
                changed = True
    return bad


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    patched = patched_functions()
    if argv[0] == "patched":
        names = kb.dump_index("pc")
        for f, spots in sorted(patched.items()):
            print(f"{f:08X} {names.get(f, ('', '?'))[1][:44]:44s} {[(hex(a), n) for a, n in spots][:4]}")
        return 0
    if argv[0] == "exclude":
        cand = {int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+|\b[0-9A-Fa-f]{6,8}\b", Path(argv[1]).read_text())}
        for a in sorted(exclusions(cand, set(patched))):
            print(f"0x{a:08X}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
