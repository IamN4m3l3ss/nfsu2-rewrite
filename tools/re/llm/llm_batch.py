#!/usr/bin/env python3
"""Unattended rewrite loop with the local model (Ollama): for each small leaf function, the model writes the C++
rewrite + its leaf.h diff test from the disassembly; we compile, hot-reload, run the diff test in the game, and feed
compile errors / mismatches back (up to --tries). Only functions whose test passes stay in the tree; nothing is
enabled - promote the passed list afterwards (tools/re/promote.py).

  llm_batch.py ADDR_LIST [--max N] [--tries K] [--workers W] [--api URL] [--model NAME] [--bench TAG]
ADDR_LIST: lines "ADDR SIZE SYSTEM [fp] NAME" (hex ADDR), e.g. from the leaf scan. Needs the game + probe running.
--api: an OpenAI-compatible server (llama-server: http://127.0.0.1:8080/v1); default: Ollama on :11434.
--workers: drafting requests in flight (match the server's --parallel). Testing is serial: one game instance; a
  single tester thread takes every draft waiting, builds once, reloads, tests each.
--bench TAG: measure only - passed drafts are deleted too, state/passed lists untouched; results in
  work/llm/bench_TAG.json (passes, tries, draft seconds, tokens/s).
State: work/llm/state.json (done/failed per address), log work/llm/log.txt, passed work/llm/passed.txt,
live status work/llm/status.json (dashboard.py).
"""
import argparse
import queue
import threading
import json
import os
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
  If a path leaves EAX untouched (or changes only AL/AX of the caller's value), add a LAST parameter
  `uint32_t eax_in` and register with LEAF_THIS_EAX / LEAF_CDECL_EAX / LEAF_STD_EAX (same arguments as LEAF_THIS);
  return eax_in on those paths, e.g. `(eax_in & 0xFFFFFF00) | al`. The LEAF_FN test entry stays the same.
- Declare `this` as `uint8_t* s` if you do byte arithmetic on it (no arithmetic on void*).
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
  {'p', k} pointer to block k, {'f', 0} float, {'r', 0} any dword. Link offsets are byte offsets
  inside the FROM block (never game addresses like 0x88A334 - read globals with gf/gu instead). Test name: llm_<ADDR> (6 hex digits, upper).
- Test table entry: LEAF_FN(name, CONV, RET, args...) with CONV one of THIS/CDECL/STD and RET always given,
  exactly one of INT/PTR/FLT/NONE, then one {kind, v} per stack argument (none for 0 args).
- Functions that read game globals (absolute addresses) may read them; never write game globals.
- Name your C++ function <Name>_<ADDR> (e.g. IsBlinking_5EBFA0): other files already use the plain names.
- End LEAF_THIS(...) / LEAF_CDECL(...) / LEAF_STD(...) lines with a semicolon.
- Comments: at most one short line per block. Do your reasoning before writing (or in your thinking), not in the
  code; long comments only cost time.
- Keep the comment header line: "// 0x<ADDR8> <name>, written from the PC machine code (local-model draft, diff-tested)."
"""


def chat(o, messages):
    """-> (text, completion tokens, seconds)"""
    t = time.time()
    if o.api:
        body = {"model": o.model, "messages": messages, "temperature": 0.2, "max_tokens": o.max_tokens,
                "chat_template_kwargs": {"enable_thinking": bool(o.thinking), **json.loads(o.template_kwargs or "{}")}}
        req = urllib.request.Request(o.api.rstrip("/") + "/chat/completions", json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=1800) as r:
            j = json.loads(r.read())
        text = j["choices"][0]["message"].get("content") or ""
        if j["choices"][0].get("finish_reason") == "length":
            text += "\n[TRUNCATED]"
        toks = j.get("usage", {}).get("completion_tokens", 0)
    else:
        body = {"model": o.model, "messages": messages, "stream": False, "think": False,
                "options": {"temperature": 0.2, "num_ctx": 16384}}
        req = urllib.request.Request(OLLAMA, json.dumps(body).encode(), {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=1800) as r:
            j = json.loads(r.read())
        text, toks = j["message"]["content"], j.get("eval_count", 0)
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S)
    return text, toks, time.time() - t


def disasm(pe, md, a, nxt):
    code = pe.get_data(a - 0x400000, min(nxt - a, 0x400))
    lines = []
    for x in md.disasm(code, a):
        lines.append(f"{x.address:06X}: {x.mnemonic} {x.op_str}".rstrip())
    while lines and re.search(r": (nop|int3)", lines[-1]):
        lines.pop()
    return "\n".join(lines)


def re_context(a):
    """EA name/signature + Ghidra's decompilation of the PC function (RE kit, tools/re/kb.py), or ''."""
    try:
        out = subprocess.run([sys.executable, str(ROOT / "tools/re/kb.py"), f"0x{a:06X}", "--pc", "--gc"],
                             capture_output=True, text=True, timeout=60).stdout
    except Exception:  # noqa: BLE001
        return ""
    lines = out.splitlines()
    head = [ln.strip() for ln in lines if ln.strip().startswith(("pc dump", "GC match", "undefined", "void", "int", "bool",
                                                                 "float", "uint")) and "[" in ln][:2]
    names = [ln.strip() for ln in lines if ln.strip().startswith(("pc dump", "GC match"))]
    pc, on = [], False
    for ln in lines:
        if re.match(r"// .* @ 00[0-9a-f]{6}$", ln):
            on = True
        elif re.match(r"// .* @ 80[0-9a-f]{6}$", ln):
            break
        if on:
            pc.append(ln)
    if not pc:
        return ""
    txt = "\n".join(names + head) + "\n\nGhidra decompilation of this PC function (a HINT: types and names may be " \
        "wrong; the machine code above is the truth for float precision, EAX contents, signed/unsigned compares and " \
        "stack cleanup):\n" + "\n".join(pc[:120])
    return txt


LOCK = threading.Lock()


def log(msg):
    WORK.mkdir(parents=True, exist_ok=True)
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    with LOCK:
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
        except Exception:  # noqa: BLE001
            pass
        time.sleep(2)
    return False


def run_test(test):
    r = pc.call("diff_test", {"name": test, "iterations": 20000, "seed": 4242}, timeout=180)
    if isinstance(r, list):
        r = r[0] if r else {"error": "test not found"}
    if isinstance(r, dict) and "results" in r:
        r = r["results"][0] if r["results"] else {"error": "test not found"}
    return r


class Run:
    def __init__(self, o, jobs):
        self.o, self.jobs = o, jobs
        self.draftq, self.testq, self.retryq = queue.Queue(), queue.Queue(), queue.Queue()
        self.workers = {}          # worker id -> text
        self.tester = "idle"
        self.open = len(jobs)
        self.t0 = time.time()
        self.stats = {"passed": 0, "gave_up": 0, "drafts": 0, "tokens": 0, "draft_s": 0.0, "tries_of_passes": 0}
        self.results = {}

    def status(self):
        el = time.time() - self.t0
        st = dict(self.stats, workers=self.workers, tester=self.tester, draft_queue=self.draftq.qsize(),
                  test_queue=self.testq.qsize(), retry_queue=self.retryq.qsize(),
                  first_drafts=sum(1 for j in self.jobs if j["tries"] > 0), open=self.open, total=len(self.jobs), elapsed_s=round(el),
                  passes_per_hour=round(self.stats["passed"] * 3600 / el, 1) if el > 60 else 0,
                  tok_s=round(self.stats["tokens"] / self.stats["draft_s"], 1) if self.stats["draft_s"] else 0,
                  model=self.o.model, bench=self.o.bench or "", pid=os.getpid(), tries=self.o.tries)
        (WORK / "status.json").write_text(json.dumps(st))

    def finish(self, j, ok):
        with LOCK:
            self.open -= 1
            self.results[j["key"]] = {"pass": ok, "tries": j["tries"], "name": j["name"]}
            self.stats["passed" if ok else "gave_up"] += 1
            if ok:
                self.stats["tries_of_passes"] += j["tries"]
        if self.open == 0:
            for _ in self.workers:
                self.draftq.put(None)

    def keep_failed(self, j, why):
        d = WORK / "failed"
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{j['key']}_try{j['tries']}.cpp").write_text(f"// {self.o.model}: {why}\n" + j.get("src", ""))

    def retry(self, j, msg):
        j["msgs"].append({"role": "user", "content": msg})
        if j["tries"] >= self.o.tries:
            log(f"{j['key']} gave up after {j['tries']} tries")
            self.finish(j, False)
        else:
            self.retryq.put(j)  # retries first: functions finish early instead of all at the end

    def worker(self, wid):
        while True:
            self.workers[wid] = "waiting"
            try:
                j = self.retryq.get_nowait()
            except queue.Empty:
                try:
                    j = self.draftq.get(timeout=1)
                except queue.Empty:
                    continue
            if j is None:
                self.workers[wid] = "done"
                return
            j["tries"] += 1
            self.workers[wid] = f"drafting {j['key']} {j['name']} (try {j['tries']})"
            try:
                out, toks, secs = chat(self.o, j["msgs"])
            except Exception as e:  # noqa: BLE001
                log(f"{j['key']} model error {e}")
                j["tries"] -= 1
                time.sleep(10)
                self.draftq.put(j)
                continue
            with LOCK:
                self.stats["drafts"] += 1
                self.stats["tokens"] += toks
                self.stats["draft_s"] += secs
            j["msgs"].append({"role": "assistant", "content": out})
            m = re.search(r"```(?:cpp|c\+\+)?\n(.*?)```", out, re.S)
            src = m.group(1) if m else ""
            if "[TRUNCATED]" in out:
                j["src"] = out
                self.keep_failed(j, "rejected: too long (hit the token cap)")
                log(f"{j['key']} draft {j['tries']} rejected (too long) {secs:.0f}s")
                self.retry(j, "Your answer was cut off: too long. Write ONLY the .cpp file, short comments, no "
                              "explanation before or after it.")
                continue
            why = ("no ```cpp code block" if not m else f"missing NFSU2_DIFF_TEST(llm_{j['key']})"
                   if f"llm_{j['key']}" not in src else "calls game code (game_fn)" if "game_fn" in src
                   else "inline asm" if re.search(r"\b(?:__asm|asm)\b", src) else "")
            if why:
                j["src"] = out
                self.keep_failed(j, "rejected: " + why)
                log(f"{j['key']} draft {j['tries']} rejected (format: {why}) {secs:.0f}s")
                self.retry(j, f"The file must contain NFSU2_DIFF_TEST(llm_{j['key']}), no game_fn calls and no "
                              "inline asm. Write the whole file again.")
                continue
            j["src"] = src
            log(f"{j['key']} draft {j['tries']} {secs:.0f}s {toks / secs if secs else 0:.0f} tok/s w{wid} {j['name']}")
            self.testq.put(j)

    def test_loop(self):
        while self.open > 0:
            try:
                first = self.testq.get(timeout=5)
            except queue.Empty:
                self.status()
                continue
            time.sleep(3)  # let drafts finishing at the same moment join this build
            batch = [first]
            while True:
                try:
                    batch.append(self.testq.get_nowait())
                except queue.Empty:
                    break
            for j in batch:
                j["path"].parent.mkdir(parents=True, exist_ok=True)
                j["path"].write_text(j["src"])
            self.tester = f"building {len(batch)} drafts"
            self.status()
            live = list(batch)
            while live:
                ok, out = build()
                if ok:
                    break
                ol = out.splitlines()
                # compiler errors name the .cpp, linker errors (duplicate symbol) the .cpp.obj on a ">>>" line
                bad = [j for j in live if any(j["path"].name in ln and ("error" in ln or ">>>" in ln) for ln in ol)]
                if not bad:
                    log("build failed outside the drafts:\n" + out[-1500:])
                    for j in live:
                        j["path"].unlink(missing_ok=True)
                    self.outside_fails = getattr(self, "outside_fails", 0) + 1
                    if self.outside_fails >= 3:
                        os._exit(1)
                    for j in live:  # not the drafts' fault: test them again later, no try used
                        self.testq.put(j)
                    live = []
                    time.sleep(20)
                    break
                for j in bad:
                    errs = [ln for ln in ol if j["path"].name in ln and "error" in ln]
                    for k, ln in enumerate(ol):  # linker: the error line comes before the ">>> defined at" lines
                        if ">>>" in ln and j["path"].name in ln:
                            e = next((ol[i] for i in range(k, max(k - 4, -1), -1) if "error" in ol[i]), "")
                            if e and e not in errs:
                                errs.append(e + "  (another file already defines this name: rename your function)")
                    j["path"].unlink(missing_ok=True)
                    live.remove(j)
                    self.keep_failed(j, "\n// ".join(errs[:6]))
                    first = re.sub(r"^.*?error: ", "", errs[0]) if errs else "?"
                    log(f"{j['key']} compile error: {first[:160]}")
                    with LOCK, (WORK / "errors.txt").open("a") as f:
                        f.write(f"== {j['key']} {self.o.model}\n" + "\n".join(errs[:12]) + "\n")
                    self.retry(j, "Compile errors:\n" + "\n".join(errs[:12]) + "\nFix them and write the whole file again.")
            if not live:
                continue
            self.outside_fails = 0
            names = set()
            for j in live:
                names |= set(re.findall(r"LEAF_(?:THIS|CDECL|STD)\(0x[0-9A-Fa-f]+, (\w+)", j["src"]))
            self.tester = f"reloading, then testing {len(live)}"
            self.status()
            if not wait_registered(names, 60):
                log("new rewrites not loaded after 60 s: forcing a game-DLL reload")
                try:
                    pc.call("rewrite_reload", timeout=60)
                except Exception as e:  # noqa: BLE001
                    log(f"rewrite_reload: {e}")
                if not wait_registered(names, 120):
                    log("still not loaded: these drafts go back to the test queue")
                    for j in live:
                        self.testq.put(j)
                    time.sleep(10)
                    continue
            for j in live:
                self.tester = f"testing {j['key']} {j['name']}"
                self.status()
                try:
                    r = run_test(f"llm_{j['key']}")
                except Exception as e:  # noqa: BLE001
                    r = {"error": f"probe: {e}"}
                if "no diff test" in str(r.get("error", "")) or "not found" in str(r.get("error", "")):
                    log(f"{j['key']} test not loaded yet: back to the test queue (not a try)")
                    self.testq.put(j)
                    continue
                if r.get("error") or r.get("mismatches", 1) or not r.get("iterations"):
                    why = r.get("error") or "; ".join(r.get("examples", [])[:4]) or "no iterations ran"
                    log(f"{j['key']} test FAIL: {why[:200]}")
                    self.keep_failed(j, why[:600])
                    j["path"].unlink(missing_ok=True)
                    self.retry(j, "The diff test against the original failed: " + why
                               + "\n(mem[k+0xOFF] = first differing dword in block k, original/rewrite; "
                               "'eax'/'st0' = return value; if the ORIGINAL crashed, a pointer it reads is not "
                               "linked in the Spec or an index is out of range.) Fix and write the whole file again.")
                    continue
                log(f"{j['key']} PASS ({r['iterations']} cases, try {j['tries']}) {j['name']}")
                if self.o.bench:
                    j["path"].unlink(missing_ok=True)
                else:
                    with LOCK:
                        with (WORK / "passed.txt").open("a") as f:
                            f.write(f"{j['key']} {j['path'].relative_to(ROOT)} {j['name']}\n")
                self.finish(j, True)
            self.tester = "idle"
            self.status()
        self.tester = "done"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("list")
    ap.add_argument("--max", type=int, default=30)
    ap.add_argument("--tries", type=int, default=3)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--api", default="")
    ap.add_argument("--model", default="qwen3.6:35b-a3b-coding")
    ap.add_argument("--bench", default="")
    ap.add_argument("--thinking", action="store_true", help="let reasoning models think first (needs a big --max-tokens)")
    ap.add_argument("--context", action="store_true", help="add EA names + Ghidra decompilation from the RE kit")
    ap.add_argument("--max-tokens", type=int, default=2500, help="answer cap (reasoning models: thinking counts)")
    ap.add_argument("--template-kwargs", default="", help='extra chat template args, e.g. \'{"reasoning_effort":"low"}\'')
    o = ap.parse_args(argv)
    WORK.mkdir(parents=True, exist_ok=True)
    sf = WORK / "state.json"
    state = json.loads(sf.read_text()) if sf.exists() else {}
    while subprocess.run(["pgrep", "-f", "fe_boot_ab[.]py"], capture_output=True).returncode == 0:
        time.sleep(20)  # a menu A/B run owns the game
    pe = xrefs.pe()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    names = kb.dump_index("pc")
    idx = sorted(names)
    nxt = {a: idx[i + 1] for i, a in enumerate(idx[:-1])}
    examples = "\n\n".join((HERE / f).read_text() for f in ("example.cpp", "example2.cpp"))
    jobs = []
    for line in Path(o.list).read_text().splitlines():
        p = line.split()
        if len(p) < 4 or (not o.bench and p[0] in state):
            continue
        a = int(p[0], 16)
        hint = names.get(a, ("", " ".join(p[3:])))[1]
        msgs = [{"role": "system", "content": RULES + "\n\nExamples:\n```cpp\n" + examples + "\n```"},
                {"role": "user", "content": f"Function 0x{a:08X} ({hint}). Machine code:\n"
                                            f"{disasm(pe, md, a, nxt.get(a, a + 0x80))}\n\n"
                                            + (re_context(a) + "\n\n" if o.context else "") + "Write the file."}]
        jobs.append({"key": f"{a:06X}", "name": hint, "msgs": msgs, "tries": 0,
                     "path": ROOT / "game" / "systems" / p[2] / "llm" / f"f_{a:06X}.cpp"})
        if len(jobs) >= o.max:
            break
    if not jobs:
        print("nothing to do")
        return 0
    run = Run(o, jobs)
    log(f"start: {len(jobs)} functions, {o.workers} workers, model {o.model}" + (f", bench {o.bench}" if o.bench else ""))
    for j in jobs:
        run.draftq.put(j)
    ths = [threading.Thread(target=run.worker, args=(w,), daemon=True) for w in range(o.workers)]
    for t in ths:
        t.start()
    run.test_loop()
    run.status()
    if not o.bench:
        for k, v in run.results.items():
            state[k] = "pass" if v["pass"] else "fail"
        sf.write_text(json.dumps(state, indent=0))
        build()  # the DLL without the given-up drafts
    else:
        st = json.loads((WORK / "status.json").read_text())
        st["results"] = run.results
        (WORK / f"bench_{o.bench}.json").write_text(json.dumps(st, indent=1))
    log(f"round done: {run.stats['passed']} of {len(jobs)} passed in {(time.time() - run.t0) / 60:.0f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
