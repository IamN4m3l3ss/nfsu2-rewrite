#!/usr/bin/env python3
"""Register contract of a function, from its call sites: which of eax/ecx/edx (and st0) callers READ after the call
before writing them. A rewrite must leave those registers exactly as the original does (e.g. return the out pointer
in eax even when the C prototype looks void). Usage: regcontract.py ADDR [ADDR...]

Implicit use counts too: if the next instruction that matters is a direct call and the caller hasn't rewritten ECX/EDX,
a callee that reads ECX/EDX before writing them (thiscall `this`) is reading the value our function left behind
(found 2026-11-15: 0x52A7A5 calls 0x503550 then 0x5036C0 on the same ECX)."""
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


_entry_cache = {}


def real_reads(ins, r):
    """Registers an instruction really reads: idioms that only write (xor/sub/sbb reg,reg; or reg,-1; and reg,0)
    lea reg,[reg] (padding) and `push ecx`/`push edx` (MSVC reserves a stack slot that way, e.g. before fstp [esp]) don't count."""
    ops = ins.op_str.replace(" ", "").split(",")
    if ins.mnemonic in ("xor", "sub", "sbb") and len(ops) == 2 and ops[0] == ops[1]:
        return []
    if len(ops) == 2 and ((ins.mnemonic == "or" and ops[1] in ("-1", "0xffffffff")) or
                          (ins.mnemonic == "and" and ops[1] == "0")):
        return []
    if ins.mnemonic == "push" and ops[0] in ("ecx", "edx"):
        return []
    if ins.mnemonic == "lea" and len(ops) == 2 and ops[1] in (f"[{ops[0]}]", f"[{ops[0]}+0]"):
        return []  # lea ecx,[ecx]: alignment padding (multi-byte nop)
    return r


def entry_reads(target, max_ins=16):
    """Registers among ecx/edx a function reads before writing them (from its entry, straight-line)."""
    if target in _entry_cache:
        return _entry_cache[target]
    base, data = xrefs.text()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    written, used = set(), set()
    off = target - base
    if 0 <= off < len(data):
        for n, ins in enumerate(md.disasm(data[off:off + 96], target)):
            r, w = ins.regs_access()
            ops = ins.op_str.replace(" ", "").split(",")
            r = real_reads(ins, r)
            for reg in r:
                name = REGS.get(reg)
                if name in ("ecx", "edx") and name not in written:
                    used.add(name)
            for reg in w:
                name = REGS.get(reg)
                if name:
                    written.add(name)
            if ins.mnemonic.startswith(("j", "call", "ret")) or n >= max_ins:
                break
    _entry_cache[target] = used
    return used


def reads_after(site, max_ins=40):
    """Registers read after the call at `site` before being written, on any path (both sides of conditional jumps,
    unconditional jumps followed) up to max_ins instructions per path; a following direct call counts the registers
    its callee reads on entry, then that path ends (the callee may change them)."""
    base, data = xrefs.text()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    used = set()
    seen = set()
    work = [(site + 5, frozenset(), 0)]
    while work:
        addr, written, n = work.pop()
        if (addr, written) in seen or not 0 <= addr - base < len(data):
            continue
        seen.add((addr, written))
        written = set(written)
        for ins in md.disasm(data[addr - base:addr - base + 160], addr):
            n += 1
            r, w = ins.regs_access()
            for reg in real_reads(ins, r):
                name = REGS.get(reg)
                if name and name not in written:
                    used.add(name)
            for reg in w:
                name = REGS.get(reg)
                if name:
                    written.add(name)
            m = ins.mnemonic
            if m == "call":
                if ins.size == 5 and data[ins.address - base] == 0xE8:
                    tgt = ins.address + 5 + struct.unpack_from("<i", data, ins.address - base + 1)[0]
                    used |= {r for r in entry_reads(tgt) if r not in written}
                break
            if m.startswith("ret") or len(written) == 3 or n >= max_ins:
                break
            if m.startswith("j"):
                tgt = ins.operands[0].imm if ins.operands and ins.operands[0].type == x86.X86_OP_IMM else None
                if tgt is not None:
                    work.append((tgt, frozenset(written), n))
                if m == "jmp":
                    break
                work.append((ins.address + ins.size, frozenset(written), n))
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
