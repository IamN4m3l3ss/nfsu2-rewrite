#!/usr/bin/env python3
"""Static helpers over SPEED2.EXE (pefile + capstone).

  xrefs.py imp NAME [--before N] [--after N]   calls through the import slot of NAME (e.g. RegisterClassExA)
  xrefs.py dis ADDR [COUNT]                    disassemble COUNT instructions at ADDR
  xrefs.py refs ADDR                           instructions that reference ADDR as an immediate/displacement
"""
import os
import struct
import sys
from functools import lru_cache
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[2]


def game_exe() -> str:
    env = {}
    for line in (ROOT / ".env.local").read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"')
    return os.path.join(env["GAME_DIR"], "SPEED2.EXE")


@lru_cache(None)
def pe():
    return pefile.PE(game_exe(), fast_load=False)


@lru_cache(None)
def text():
    p = pe()
    s = next(s for s in p.sections if s.Name.rstrip(b"\0") == b".text")
    return p.OPTIONAL_HEADER.ImageBase + s.VirtualAddress, s.get_data()


def imports() -> dict:
    out = {}
    for e in pe().DIRECTORY_ENTRY_IMPORT:
        for i in e.imports:
            if i.name:
                out[i.name.decode()] = i.address
    return out


def md():
    m = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    m.detail = False
    return m


def dis(addr: int, count: int = 30) -> list[str]:
    base, data = text()
    off = addr - base
    return [f"{i.address:08x}  {i.mnemonic:<6} {i.op_str}" for _, i in zip(range(count), md().disasm(data[off:off + count * 16], addr))]


def find_calls_through(slot: int) -> list[int]:
    """call [slot] (FF 15) and jmp [slot] (FF 25); also mov reg,[slot] (8B 05/0D/15/1D/35/3D, A1)."""
    base, data = text()
    needle = struct.pack("<I", slot)
    hits = []
    i = data.find(needle)
    while i != -1:
        for pre_len, prefixes in ((2, (b"\xff\x15", b"\xff\x25", b"\x8b\x05", b"\x8b\x0d", b"\x8b\x15", b"\x8b\x1d",
                                       b"\x8b\x35", b"\x8b\x3d")), (1, (b"\xa1",))):
            if i >= pre_len and data[i - pre_len:i] in prefixes:
                hits.append(base + i - pre_len)
        i = data.find(needle, i + 1)
    return hits


def context(addr: int, before: int, after: int) -> list[str]:
    # disassemble from a bit earlier and resync on addr
    base, data = text()
    for back in range(before * 7, 0, -1):
        start = addr - back
        lines = list(md().disasm(data[start - base:addr - base + after * 16], start))
        idx = next((k for k, ins in enumerate(lines) if ins.address == addr), None)
        if idx is not None and idx >= before:
            sel = lines[idx - before: idx + after + 1]
            return [f"{'>' if ins.address == addr else ' '}{ins.address:08x}  {ins.mnemonic:<6} {ins.op_str}" for ins in sel]
    return dis(addr, after)


def main(a: list[str]) -> int:
    if not a:
        print(__doc__); return 2
    opt = lambda k, d: int(a[a.index(k) + 1]) if k in a else d
    if a[0] == "imp":
        slot = imports()[a[1]]
        hits = find_calls_through(slot)
        print(f"{a[1]} slot={slot:#x}: {len(hits)} reference(s)")
        for h in hits:
            print("\n".join(context(h, opt("--before", 8), opt("--after", 4))))
            print("-" * 60)
    elif a[0] == "dis":
        print("\n".join(dis(int(a[1], 0), int(a[2]) if len(a) > 2 else 30)))
    elif a[0] == "refs":
        target = int(a[1], 0)
        base, data = text()
        needle = struct.pack("<I", target)
        i = data.find(needle)
        while i != -1:
            for back in range(1, 7):
                ins = next(md().disasm(data[i - back:i - back + 16], base + i - back), None)
                if ins and ins.address + ins.size >= base + i + 4 and ins.address <= base + i:
                    print(f"{ins.address:08x}  {ins.mnemonic:<6} {ins.op_str}")
                    break
            i = data.find(needle, i + 1)
    else:
        print(__doc__); return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
