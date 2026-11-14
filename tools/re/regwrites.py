#!/usr/bin/env python3
"""Which of eax/ecx/edx a function writes (scans its body up to the next function start; follows tail jumps to
another function by including that function too). Registers it never writes are, in practice, preserved for its
callers (MSVC's whole-program optimisation may rely on that), so a rewrite must preserve them as well.
Usage: regwrites.py ADDR [ADDR...]"""
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, __import__("os").path.dirname(__file__))
import xrefs  # noqa: E402
from regcontract import REGS  # noqa: E402

_starts = None


def starts():
    global _starts
    if _starts is None:
        base, data = xrefs.text()
        s = set()
        for i in range(len(data) - 5):
            if data[i] == 0xE8:
                t = base + i + 5 + struct.unpack_from("<i", data, i + 1)[0]
                if base <= t < base + len(data):
                    s.add(t)
        _starts = sorted(s)
    return _starts


def body(addr):
    st = starts()
    nxt = next((x for x in st if x > addr), addr + 0x400)
    base, data = xrefs.text()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return list(md.disasm(data[addr - base:nxt - base], addr)), nxt


def writes(addr, seen=None):
    seen = seen or set()
    if addr in seen:
        return set(), []
    seen.add(addr)
    ins, end = body(addr)
    w, calls = set(), []
    for i in ins:
        _, wr = i.regs_access()
        for reg in wr:
            if REGS.get(reg):
                w.add(REGS[reg])
        if i.mnemonic == "call":
            calls.append(i.op_str)
        if i.mnemonic == "jmp" and i.op_str.startswith("0x"):
            t = int(i.op_str, 16)
            if not (addr <= t < end):  # tail jump into another function
                w2, c2 = writes(t, seen)
                w |= w2
                calls += c2
    return w, calls


if __name__ == "__main__":
    for a in [int(x, 0) for x in sys.argv[1:]]:
        w, calls = writes(a)
        keep = sorted({"ecx", "edx"} - w)
        print(f"{a:#010x}: writes {sorted(w)}; calls {calls or '-'}; PRESERVE {keep or '-'}")
