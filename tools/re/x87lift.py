#!/usr/bin/env python3
"""Lift straight-line x87 float code (MSVC 2003-style) to equivalent C++.

Simulates the x87 stack and the integer registers symbolically and emits C++ that does the same loads, float
operations and stores in the same order. On the game thread the x87 runs at 24-bit precision (measured), so each x87
arithmetic op rounds like one C++ `float` op: the emitted code is meant to be bit-exact. Always diff-test the result.

Usage: x87lift.py ADDR [--name NAME]     (stops at the first `ret`; refuses on branches/unsupported instructions)

Memory model: [esp+N] at entry -> stack args (arg0 = [esp+4]); locals live in a frame relative to the entry esp.
Pointers loaded from args are named pK; memory through them is accessed as raw bytes (`p0 + 0x14`).
"""
from __future__ import annotations

import re
import struct
import sys

import capstone

sys.path.insert(0, __import__("os").path.dirname(__file__))
import xrefs  # noqa: E402

MEMRE = re.compile(r"(byte|word|dword|qword) ptr \[(.+)\]")


class Lifter:
    def __init__(self, addr, name):
        self.start, self.name = addr, name
        self.esp = 0                  # current esp relative to entry esp (entry: [esp] = return address)
        self.regs = {"esp": ("esp",)}  # symbolic int registers: ("arg", k) | ("ptr", k) | ("int", expr) | ("val", c)
        self.fpu = []                 # x87 stack, index 0 = st(0); entries are C expression strings (float)
        self.frame = {}               # entry-relative stack offset -> stored symbolic value (float expr / int)
        self.out = []                 # emitted C++ lines
        self.tmp = 0
        self.ptrs = {}                # arg index -> pointer name
        self.maxarg = -1
        self.raw = set()              # float temps that are straight loads (stored as-is -> x87 quiets SNaN)

    # ---------------------------------------------------------------- helpers
    def t(self, expr, kind="float"):
        name = f"t{self.tmp}"
        self.tmp += 1
        self.out.append(f"    const {kind} {name} = {expr};")
        return name

    def const_float(self, absaddr):
        p = xrefs.pe()
        v = struct.unpack("<f", p.get_data(absaddr - p.OPTIONAL_HEADER.ImageBase, 4))[0]
        u = struct.unpack("<I", p.get_data(absaddr - p.OPTIONAL_HEADER.ImageBase, 4))[0]
        return f"f32(0x{u:08X}u) /* {v!r} @ {absaddr:#x} */"

    def addr(self, inner):
        """Resolve a memory operand -> ('frame', off) | ('ptr', name, off) | ('abs', a)."""
        inner = inner.replace(" ", "")
        m = re.fullmatch(r"(e[a-z]{2})(?:([+-])(0x[0-9a-f]+|\d+))?", inner)
        if m:
            reg, sign, off = m.group(1), m.group(2), m.group(3)
            off = int(off, 0) if off else 0
            off = -off if sign == "-" else off
            if reg == "esp":
                return ("frame", self.esp + off)
            r = self.regs.get(reg)
            if r and r[0] == "ptr":
                return ("ptr", r[1], off)
            raise ValueError(f"memory through non-pointer register {reg}: {r}")
        m = re.fullmatch(r"0x[0-9a-f]+", inner)
        if m:
            return ("abs", int(inner, 16))
        raise ValueError(f"unsupported addressing [{inner}]")

    def frame_arg(self, off):
        # entry-relative offset 4,8,... are args 0,1,...
        if off >= 4 and (off - 4) % 4 == 0:
            k = (off - 4) // 4
            self.maxarg = max(self.maxarg, k)
            return k
        return None

    def load_float(self, a):
        if a[0] == "frame":
            if a[1] in self.frame:
                v = self.frame[a[1]]
                return v if v[0] == "f" else ("f", f"bitsf({v[1]})")
            k = self.frame_arg(a[1])
            if k is None:
                raise ValueError(f"read of unwritten local {a[1]}")
            return ("f", f"a{k}f")
        if a[0] == "ptr":
            t = self.t(f"ldf({a[1]}, {a[2]:#x})")
            self.raw.add(t)
            return ("f", t)
        return ("f", self.const_float(a[1]))

    def load_int(self, a):
        if a[0] == "frame":
            if a[1] in self.frame:
                v = self.frame[a[1]]
                return v if v[0] == "i" else ("i", f"fbits({v[1]})")
            k = self.frame_arg(a[1])
            if k is None:
                raise ValueError(f"read of unwritten local {a[1]}")
            return ("i", f"a{k}")
        if a[0] == "ptr":
            return ("i", self.t(f"ldi({a[1]}, {a[2]:#x})", "uint32_t"))
        raise ValueError("int load from absolute address")

    def store(self, a, v):
        if a[0] == "frame":
            k = self.frame_arg(a[1])
            if k is not None:
                self.maxarg = max(self.maxarg, k)
            self.frame[a[1]] = v
            return
        if a[0] == "ptr":
            if v[0] == "f":
                e = f"x87q({v[1]})" if v[1] in self.raw or v[1].startswith("a") and v[1].endswith("f") else v[1]
                self.out.append(f"    stf({a[1]}, {a[2]:#x}, {e});")
            else:
                self.out.append(f"    sti({a[1]}, {a[2]:#x}, {v[1]});")
            return
        raise ValueError("store to absolute address")

    def st(self, i):
        return self.fpu[i]

    # ---------------------------------------------------------------- instruction handlers
    def run(self):
        base, data = xrefs.text()
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for ins in md.disasm(data[self.start - base:self.start - base + 0x1000], self.start):
            m, ops = ins.mnemonic, ins.op_str
            try:
                if m == "ret":
                    self.finish(ins)
                    return
                self.step(m, ops)
            except Exception as e:
                raise SystemExit(f"{ins.address:#x}: {m} {ops}: {e}")
        raise SystemExit("no ret found")

    def binop(self, m, ops):
        base = m.rstrip("p")
        rev = base.endswith("r")
        op = {"fadd": "+", "fsub": "-", "fsubr": "-", "fmul": "*", "fdiv": "/", "fdivr": "/"}[base]
        if ops.startswith(("dword ptr", "qword ptr")):  # st0 = st0 op mem
            v = self.load_float(self.addr(MEMRE.match(ops).group(2)))[1]
            a, b = (v, self.st(0)) if rev else (self.st(0), v)
            self.fpu[0] = self.t(f"{a} {op} {b}")
            return
        regs = [r.strip() for r in ops.split(",")] if ops else ["st(1)", "st(0)"]
        idx = [0 if r == "st(0)" else int(r[3:-1]) for r in regs]
        dst, src = idx if len(idx) == 2 else (0, idx[0])
        if len(idx) == 1:
            # one operand: "fmul st(i)" -> st0 = st0 op st(i);  "fsubp st(i)" -> st(i) = st(i) op st0, pop.
            # (Intel-manual semantics as capstone prints them; confirmed by the hand-traced, bit-exact bSin/bSinCos.)
            dst, src = (idx[0], 0) if m.endswith("p") else (0, idx[0])
        a, b = self.st(dst), self.st(src)
        if rev:
            a, b = b, a
        self.fpu[dst] = self.t(f"{a} {op} {b}")
        if m.endswith("p"):
            assert src == 0, "p-variant must pop st(0)"
            self.fpu.pop(0)

    def step(self, m, ops):
        if m in ("nop", "int3"):
            return
        if m == "push":
            self.esp -= 4
            r = ops.strip()
            if r in self.regs:
                self.frame[self.esp] = ("i", f"/*saved {r}*/0u")
            return
        if m == "pop":
            self.esp += 4
            return
        if m in ("sub", "add") and ops.startswith("esp,"):
            n = int(ops.split(",")[1], 0)
            self.esp += -n if m == "sub" else n
            return
        if m == "and" and ops.startswith("esp,"):
            return  # stack alignment: locals are entry-relative in our model
        if m == "mov" and ops == "ebp, esp":
            self.regs["ebp"] = ("frameptr", self.esp)
            return
        if m == "xor":
            a, b = [x.strip() for x in ops.split(",")]
            if a == b:
                self.regs[a] = ("val", "0u")
                return
        if m == "mov":
            d, s = [x.strip() for x in ops.split(",", 1)]
            if d.startswith("e") and "ptr" not in d:  # load register
                if "ptr" in s:
                    a = self.addr(MEMRE.match(s).group(2))
                    if a[0] == "frame" and a[1] not in self.frame and self.frame_arg(a[1]) is not None:
                        k = self.frame_arg(a[1])
                        name = self.ptrs.setdefault(k, f"p{k}")
                        self.regs[d] = ("ptr", name)
                        return
                    self.regs[d] = self.load_int(a)
                    return
                if s.startswith("0x") or s.isdigit():
                    self.regs[d] = ("val", f"{int(s, 0):#x}u")
                    return
                self.regs[d] = self.regs[s]
                return
            a = self.addr(MEMRE.match(d).group(2))
            if s.startswith("0x") or s.isdigit():
                self.store(a, ("i", f"{int(s, 0):#x}u"))
            else:
                r = self.regs[s]
                self.store(a, ("i", r[1]) if r[0] in ("i", "val") else r)
            return
        if m == "fld":
            if ops.startswith("st("):
                self.fpu.insert(0, self.st(int(ops[3:-1])))
            else:
                self.fpu.insert(0, self.load_float(self.addr(MEMRE.match(ops).group(2)))[1])
            return
        if m == "fld1":
            self.fpu.insert(0, "1.0f")
            return
        if m == "fldz":
            self.fpu.insert(0, "0.0f")
            return
        if m == "fild":
            v = self.load_int(self.addr(MEMRE.match(ops).group(2)))[1]
            self.fpu.insert(0, self.t(f"static_cast<float>(static_cast<int32_t>({v}))"))
            return
        if m in ("fst", "fstp"):
            if ops.startswith("st("):
                i = int(ops[3:-1])
                self.fpu[i] = self.st(0)
            else:
                self.store(self.addr(MEMRE.match(ops).group(2)), ("f", self.st(0)))
            if m == "fstp":
                self.fpu.pop(0)
            return
        if m == "fxch":
            i = int(ops[3:-1]) if ops else 1
            self.fpu[0], self.fpu[i] = self.fpu[i], self.fpu[0]
            return
        if m == "fchs":
            self.fpu[0] = self.t(f"fchs({self.st(0)})")
            return
        if m == "fabs":
            self.fpu[0] = self.t(f"fabs_({self.st(0)})")
            return
        if m == "fsqrt":
            self.fpu[0] = self.t(f"__builtin_sqrtf({self.st(0)})")
            return
        if m.rstrip("p") in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr"):
            self.binop(m, ops)
            return
        raise ValueError("unsupported instruction")

    def finish(self, ins):
        args = ", ".join(f"uint32_t a{k}" for k in range(self.maxarg + 1))
        print(f"// lifted from {self.start:#x} by tools/re/x87lift.py — verify with a diff test")
        print(f"// at ret: fpu stack depth {len(self.fpu)} (st0 = {self.fpu[0] if self.fpu else '-'}); "
              f"eax={self.regs.get('eax')} ecx={self.regs.get('ecx')} edx={self.regs.get('edx')}")
        print(f"static void {self.name}_lifted({args}) {{")
        print("    using namespace lift;")
        for k in range(self.maxarg + 1):
            print(f"    const float a{k}f = bitsf(a{k}); (void)a{k}f;")
        for k, name in sorted(self.ptrs.items()):
            print(f"    uint8_t* const {name} = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(a{k}));")
        print("\n".join(self.out))
        print("}")


if __name__ == "__main__":
    a = int(sys.argv[1], 0)
    name = sys.argv[sys.argv.index("--name") + 1] if "--name" in sys.argv else f"f_{a:x}"
    Lifter(a, name).run()
