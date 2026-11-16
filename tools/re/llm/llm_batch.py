#!/usr/bin/env python3
"""Unattended rewrite loop with the local model (Ollama): for each small leaf function, the model writes the C++
rewrite + its leaf.h diff test from the disassembly; we compile, hot-reload, run the diff test in the game, and feed
compile errors / mismatches back (up to --tries). Only functions whose test passes stay in the tree; nothing is
enabled - promote the passed list afterwards (tools/re/promote.py).

  llm_batch.py ADDR_LIST [--max N] [--tries K] [--model NAME]
ADDR_LIST: lines "ADDR SIZE SYSTEM [fp] NAME" (hex ADDR), e.g. from the leaf scan. Needs the game + probe running.
State: work/llm/state.json (done/failed per address), log work/llm/log.txt, passed work/llm/passed.txt.
"""
import argparse
import json
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import capstone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools" / "re"))
sys.path.insert(0, str(ROOT / "tools" / "probe"))
import kb  # noqa: E402
import probe_client as pc  # noqa: E402
import xrefs  # noqa: E402

WORK = ROOT / "work" / "llm"
OLLAMA = "http://127.0.0.1:11434/api/chat"

RULES = """You rewrite one function of Need for Speed Underground 2 (PC, 32-bit x86, MSVC) as C++ for our hook-and-replace
project. Output ONE complete .cpp file in a single ```cpp block, following the two examples exactly in structure.

Rules:
- The rewrite must behave exactly like the machine code: same return value in EAX (or st0 for floats), same memory
  writes, same stack cleanup. It is installed through an entry stub that saves ECX/EDX, so you only write logic.
- Our function is a plain __cdecl C++ function. thiscall: first parameter `void* s` is ECX (`this`), then the stack
  args; register with LEAF_THIS(addr, name, nargs, "signature"). cdecl (plain `ret`, no ECX use): LEAF_CDECL.
  stdcall (`ret N` without ECX as this): LEAF_STD. nargs = number of 4-byte stack arguments.
- Return type: uint32_t for anything in EAX (bool, int, pointers as uint32_t via u(ptr)), float if the function
  returns in st0 (then the test uses FLT). A "void" function still returns what it leaves in EAX (look at the last
  write to EAX/AL/AX on each path; partial writes like `mov al, ..` keep the upper bytes of the earlier EAX value).
  If a path leaves EAX untouched from the caller, you cannot reproduce that: write `return 0;` there and say so in
  a comment - the test will tell.
- Helpers (game::leaf): at<T>(p, off) field reference, ptr(p, off) = pointer field, gf(addr)/gu(addr) read a game
  global float/dword, u(p) pointer -> uint32_t. Floats: plain float math matches the game's x87 (24-bit precision)
  for + - * / ; use std::fabs, comparisons as written. x87 compare: after `fcomp m` / `fcom m`: C0 (ah 0x01) = st0 < m, C3 (ah 0x40) = st0 == m, C2 (0x04) = unordered;
  NaN sets all three. `test ah, X; jp L` jumps when (ah & X) has an EVEN number of set bits (zero counts as even);
  `jnp` when odd. `test ah, X; jne/jz` = any/none of the bits set. Unordered (NaN) cases must behave the same.
- Never call game functions, never use the function's own address except in LEAF_*.
- The diff test: Spec{blocks, block_size, links, ints}. Block 0 is `this` (or point a pointer argument at a block
  with {'p', k}). Every pointer the code dereferences must be a Link {from_block, offset, to_block} (add `true` as
  4th member if the code tests it for null). IntField {block, offset, size 1/2/4, range} keeps indexes/counts/enum
  bytes small so branches are exercised and indexing stays inside the block. Block size must cover every offset
  used (max offset + index*stride + 16), multiple of 0x100, at most 0x2000. Args: {'i', n} small int 0..n-1,
  {'p', k} pointer to block k, {'f', 0} float, {'r', 0} any dword. Test name: llm_<ADDR> (6 hex digits, upper).
- Test table entry: LEAF_FN(name, CONV, RET, args...) with CONV one of THIS/CDECL/STD and RET always given,
  exactly one of INT/PTR/FLT/NONE, then one {kind, v} per stack argument (none for 0 args).
- Functions that read game globals (absolute addresses) may read them; never write game globals.
- Keep the comment header line: "// 0x<ADDR8> <name>, written from the PC machine code (local-model draft, diff-tested)."
"""


def ollama(model, messages):
    body = json.dumps({"model": model, "messages": messages, "stream": False, "think": False,
                       "options": {"temperature": 0.2, "num_ctx": 16384}}).encode()
    req = urllib.request.Request(OLLAMA, body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=900) as r:
        return json.loads(r.read())["message"]["content"]


def disasm(pe, md, a, nxt):
    code = pe.get_data(a - 0x400000, min(nxt - a, 0x400))
    lines = []
    for x in md.disasm(code, a):
        lines.append(f"{x.address:06X}: {x.mnemonic} {x.op_str}".rstrip())
    while lines and re.search(r": (nop|int3)", lines[-1]):
        lines.pop()
    return "\n".join(lines)


def log(msg):
    WORK.mkdir(parents=True, exist_ok=True)
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    with (WORK / "log.txt").open("a") as f:
        f.write(line + "\n")


def build():
    r = subprocess.run(["cmake", "--build", "--preset", "win32", "--target", "nfsu2_game"], cwd=ROOT,
                       capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


def wait_registered(names, timeout=90):
    t = time.time()
    while time.time() - t < timeout:
        try:
            have = {x["name"] for x in pc.call("rewrite_list")["rewrites"]}
            if names <= have:
                return True
        except Exception:
            pass
        time.sleep(3)
    return False


def run_test(test):
    r = pc.call("diff_test", {"name": test, "iterations": 20000, "seed": 4242}, timeout=180)
    if isinstance(r, list):
        r = r[0] if r else {"error": "test not found"}
    if isinstance(r, dict) and "results" in r:
        r = r["results"][0] if r["results"] else {"error": "test not found"}
    return r


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("list")
    ap.add_argument("--max", type=int, default=30)
    ap.add_argument("--tries", type=int, default=3)
    ap.add_argument("--model", default="qwen3.6:35b-a3b-coding")
    o = ap.parse_args(argv)
    WORK.mkdir(parents=True, exist_ok=True)
    sf = WORK / "state.json"
    state = json.loads(sf.read_text()) if sf.exists() else {}
    while subprocess.run(["pgrep", "-f", "fe_boot_ab.py"], capture_output=True).returncode == 0:
        time.sleep(20)  # a menu A/B run owns the game
    pe = xrefs.pe()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    idx = sorted(kb.dump_index("pc"))
    nxt = {a: idx[i + 1] for i, a in enumerate(idx[:-1])}
    examples = "\n\n".join((HERE / f).read_text() for f in ("example.cpp", "example2.cpp"))
    todo = []
    for line in Path(o.list).read_text().splitlines():
        p = line.split()
        if len(p) < 4 or p[0] in state:
            continue
        a = int(p[0], 16)
        name = " ".join(p[4:] if p[3] == "fp" else p[3:])
        todo.append((a, p[2], name))
        if len(todo) >= o.max:
            break
    # conversation per function: draft -> feedback rounds
    jobs = {}
    for a, sysname, name in todo:
        asm = disasm(pe, md, a, nxt.get(a, a + 0x80))
        hint = kb.dump_index("pc").get(a, ("", name))[1]
        msgs = [{"role": "system", "content": RULES + "\n\nExamples:\n```cpp\n" + examples + "\n```"},
                {"role": "user", "content": f"Function 0x{a:08X} ({hint}). Machine code:\n{asm}\n\nWrite the file."}]
        path = ROOT / "game" / "systems" / sysname / "llm" / f"f_{a:06X}.cpp"
        jobs[a] = {"msgs": msgs, "path": path, "tries": 0, "name": hint}
    active = dict(jobs)
    while active:
        # 1. draft / redraft
        for a, j in list(active.items()):
            j["tries"] += 1
            t = time.time()
            (WORK / "current.json").write_text(json.dumps({"stage": "drafting", "addr": f"{a:06X}", "name": j["name"],
                                                          "try": j["tries"]}))
            try:
                out = ollama(o.model, j["msgs"])
            except Exception as e:  # noqa: BLE001
                log(f"{a:06X} ollama error {e}")
                del active[a]
                state[f"{a:06X}"] = "error"
                continue
            m = re.search(r"```(?:cpp|c\+\+)?\n(.*?)```", out, re.S)
            j["msgs"].append({"role": "assistant", "content": out})
            src = m.group(1) if m else ""
            if f"llm_{a:06X}" not in src or "game_fn" in src or re.search(r"\b(?:__asm|asm)\b", src):
                j["msgs"].append({"role": "user", "content": f"The file must contain NFSU2_DIFF_TEST(llm_{a:06X}), "
                                  "no game_fn calls and no inline asm. Write the whole file again."})
                j["src_ok"] = False
                log(f"{a:06X} draft {j['tries']} rejected (format) {time.time() - t:.0f}s")
                continue
            j["src_ok"] = True
            j["path"].parent.mkdir(parents=True, exist_ok=True)
            j["path"].write_text(src)
            log(f"{a:06X} draft {j['tries']} {time.time() - t:.0f}s {j['name']}")
        # 2. compile all drafts together; files with errors get the errors back
        for _ in range(len(active) + 1):
            ok, out = build()
            if ok:
                break
            bad = {}
            for a, j in active.items():
                errs = [ln for ln in out.splitlines() if j["path"].name in ln and "error" in ln]
                if errs:
                    bad[a] = errs
            if not bad:
                log("build failed outside the drafts:\n" + out[-1500:])
                return 1
            for a, errs in bad.items():
                j = active[a]
                j["path"].unlink(missing_ok=True)
                j["src_ok"] = False
                j["msgs"].append({"role": "user", "content": "Compile errors:\n" + "\n".join(errs[:12])
                                  + "\nFix them and write the whole file again."})
                log(f"{a:06X} compile error")
        # 3. test the ones that compiled
        built = {a: j for a, j in active.items() if j.get("src_ok")}
        if built:
            fnnames = set()
            for j in built.values():
                fnnames |= set(re.findall(r"LEAF_(?:THIS|CDECL|STD)\(0x[0-9A-Fa-f]+, (\w+)", j["path"].read_text()))
            if not wait_registered(fnnames):
                log("new rewrites did not show up after the reload")
        for a, j in built.items():
            r = run_test(f"llm_{a:06X}")
            if r.get("error") or r.get("mismatches", 1) or not r.get("iterations"):
                why = r.get("error") or "; ".join(r.get("examples", [])[:4]) or "no iterations ran"
                log(f"{a:06X} test FAIL: {why[:200]}")
                j["path"].unlink(missing_ok=True)
                j["msgs"].append({"role": "user", "content": "The diff test against the original failed: " + why
                                  + "\n(mem[k+0xOFF] = first differing dword in block k, original/rewrite; "
                                  "'eax'/'st0' = return value; if the ORIGINAL crashed, a pointer it reads is not "
                                  "linked in the Spec or an index is out of range.) Fix and write the whole file again."})
            else:
                log(f"{a:06X} PASS ({r['iterations']} cases) {j['name']}")
                state[f"{a:06X}"] = "pass"
                with (WORK / "passed.txt").open("a") as f:
                    f.write(f"{a:06X} {j['path'].relative_to(ROOT)} {j['name']}\n")
                del active[a]
        for a, j in list(active.items()):
            if j["tries"] >= o.tries:
                j["path"].unlink(missing_ok=True)
                state[f"{a:06X}"] = "fail"
                log(f"{a:06X} gave up after {j['tries']} tries")
                del active[a]
        sf.write_text(json.dumps(state, indent=0))
        if any(not j["path"].exists() for j in jobs.values() if state.get(f"{j['path'].stem[2:]}") == "fail"):
            ok, _ = build()  # drop the given-up files from the DLL
    (WORK / "current.json").unlink(missing_ok=True)
    n = sum(1 for a in jobs if state.get(f"{a:06X}") == "pass")
    log(f"round done: {n} of {len(jobs)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
