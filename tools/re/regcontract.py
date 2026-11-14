#!/usr/bin/env python3
"""Register contract of a function, from its call sites: which of eax/ecx/edx (and st0) callers READ after the call
before writing them. A rewrite must leave those registers exactly as the original does (e.g. return the out pointer
in eax even when the C prototype looks void). Usage: regcontract.py ADDR [ADDR...]"""
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, __import__("os").path.dirname(__file__))
import xrefs  # noqa: E402

REGS = {x86.X86_REG_EAX: "eax", x86.X86_REG_ECX: "ecx", x86.X86_REG_EDX: "edx",
        x86.X86_REG_AX: "eax", x86.X86_REG_AL: "eax", x86.X86_REG_AH: "eax",
        x86.X86_REG_CX: "ecx", x86.X86_REG_CL: "ecx", x86.X86_REG_CH: "ecx",
        x86.X86_REG_DX: "edx", x86.X86_REG_DL: "edx", x86.X86_REG_DH: "edx"}


def call_sites(target):
    base, data = xrefs.text()
    out = []
    for i in range(len(data) - 5):
        if data[i] == 0xE8 and base + i + 5 + struct.unpack_from("<i", data, i + 1)[0] == target:
            out.append(base + i)
    return out


def reads_after(site, max_ins=12):
    base, data = xrefs.text()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    written, used = set(), set()
    start = site + 5
    for ins in md.disasm(data[start - base:start - base + 64], start):
        r, w = ins.regs_access()
        ops = ins.op_str.replace(" ", "").split(",")
        if ins.mnemonic in ("xor", "sub", "sbb") and len(ops) == 2 and ops[0] == ops[1]:
            r = []  # zeroing idiom: writes only
        for reg in r:
            name = REGS.get(reg)
            if name and name not in written:
                used.add(name)
        # "add esp, n" after cdecl calls is harmless; stop at control flow
        for reg in w:
            name = REGS.get(reg)
            if name:
                written.add(name)
        if ins.mnemonic.startswith(("j", "call", "ret")) or len(written) == 3:
            break
    return used


def main(addrs):
    for a in addrs:
        sites = call_sites(a)
        counts = {}
        for s in sites:
            for r in reads_after(s):
                counts[r] = counts.get(r, 0) + 1
        print(f"{a:#010x}: {len(sites)} call sites; registers read after the call: "
              + (", ".join(f"{r} at {n} sites" for r, n in sorted(counts.items())) or "none"))


if __name__ == "__main__":
    main([int(x, 0) for x in sys.argv[1:]])
