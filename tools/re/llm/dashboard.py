#!/usr/bin/env python3
"""Live page for the local-model loop: http://127.0.0.1:8765  (progress, current log, GPU/CPU/RAM, loaded model)."""
import glob
import json
import os
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / "work" / "llm"
LIST = ROOT / "work" / "llm_core_list.txt"

PAGE = """<!doctype html><meta charset=utf-8><title>Local LLM</title>
<style>body{font:14px system-ui;background:#111;color:#ddd;margin:16px;max-width:900px}
.row{display:flex;gap:12px;flex-wrap:wrap}.c{background:#1c1c1c;border-radius:8px;padding:10px 14px;flex:1;min-width:140px}
.v{font-size:22px;color:#fff}.bar{height:8px;background:#333;border-radius:4px;margin-top:6px}
.bar i{display:block;height:8px;background:#4caf50;border-radius:4px}pre{background:#1c1c1c;padding:10px;
border-radius:8px;white-space:pre-wrap;font-size:12px;max-height:60vh;overflow:auto}</style>
<h2>Local LLM rewrite loop</h2><div class=row id=s></div><h3>Worker (one: the GPU drafts one function at a time)</h3><div class=c id=w></div>
<h3>Latest draft: <span id=fn></span></h3><pre id=code></pre><h3>Log</h3><pre id=l></pre>
<script>
function card(t,v,p){return `<div class=c>${t}<div class=v>${v}</div>`+(p!=null?`<div class=bar><i style="width:${p}%"></i></div>`:'')+`</div>`}
async function tick(){try{const d=await (await fetch('/data')).json();
document.getElementById('s').innerHTML=
card(`Try ${d.cur_try} of 3: drafted`,`${d.r_drafted} / ${d.r_size}`,100*d.r_drafted/d.r_size)
+card('This round: tests',`${d.r_pass} passed, ${d.r_fail} failed`)
+card('Speed',`${d.avg}s per draft`)+card('Round ETA',d.eta)
+card('All rounds: finished',`${d.done} / ${d.total}`,d.total?100*d.done/d.total:0)
+card('All rounds: passed',d.passed)+card('All rounds: gave up',d.failed)+card('GPU',d.gpu+'%',d.gpu)
+card('VRAM',`${d.vram_used} / ${d.vram_total} GB`,100*d.vram_used/(d.vram_total||1))
+card('CPU load',d.load)+card('RAM',`${d.ram_used} / ${d.ram_total} GB`,100*d.ram_used/d.ram_total)
+card('Model',d.model||'not loaded');
document.getElementById('w').textContent=d.worker;
document.getElementById('fn').textContent=d.draft_name;document.getElementById('code').textContent=d.draft;
const l=document.getElementById('l');l.textContent=d.log;l.scrollTop=1e9}catch(e){}}
tick();setInterval(tick,2000)</script>"""


def gpu():
    for dev in glob.glob("/sys/class/drm/card*/device"):
        try:
            busy = int(open(f"{dev}/gpu_busy_percent").read())
            used = int(open(f"{dev}/mem_info_vram_used").read()) / 2**30
            tot = int(open(f"{dev}/mem_info_vram_total").read()) / 2**30
            if tot > 4:
                return busy, round(used, 1), round(tot, 1)
        except OSError:
            pass
    return 0, 0, 0


def data():
    state = json.loads((WORK / "state.json").read_text()) if (WORK / "state.json").exists() else {}
    log = (WORK / "log.txt").read_text().splitlines()[-40:] if (WORK / "log.txt").exists() else []
    mem = {k: int(v.split()[0]) for k, v in (ln.split(":", 1) for ln in open("/proc/meminfo"))}
    model = ""
    try:
        ps = json.loads(urllib.request.urlopen("http://127.0.0.1:11434/api/ps", timeout=2).read())
        model = ", ".join(m["name"] for m in ps.get("models", []))
    except Exception:  # noqa: BLE001
        model = "ollama not reachable"
    g = gpu()
    full = (WORK / "log.txt").read_text().splitlines() if (WORK / "log.txt").exists() else []
    k = max([i for i, ln in enumerate(full) if "round done" in ln] or [-1]) + 1
    rnd = full[k:]
    tries = [int(ln.split()[3]) for ln in rnd if " draft " in ln]
    cur_try = max(tries) if tries else 1
    drafted = {ln.split()[1] for ln in rnd if f" draft {cur_try} " in ln}
    finished = {ln.split()[1] for ln in rnd if " PASS " in ln or "gave up" in ln}
    secs = [int(ln.split()[4][:-1]) for ln in rnd if " draft " in ln and ln.split()[4].endswith("s")]
    avg = round(sum(secs) / len(secs)) if secs else 0
    r_size = 30 - len(finished) if cur_try > 1 else 30
    left = max(r_size - len(drafted), 0)
    eta = f"~{left * avg // 60} min of drafting, then build + tests" if left else "building / testing / retries"
    drafts = sorted(glob.glob(str(ROOT / "game/systems/*/llm/f_*.cpp")), key=os.path.getmtime)
    draft = Path(drafts[-1]) if drafts else None
    last = log[-1] if log else ""
    worker = f"last step: {last}" if last else "idle"
    cur = WORK / "current.json"
    if cur.exists():
        c = json.loads(cur.read_text())
        worker = f"{c['stage']}: {c['addr']} {c['name']} (try {c['try']})  |  last step: {last}"
    total = sum(1 for ln in LIST.read_text().splitlines() if ln.strip()) if LIST.exists() else 0
    return {"done": len(state), "total": total, "passed": sum(v == "pass" for v in state.values()),
            "failed": sum(v != "pass" for v in state.values()), "log": "\n".join(log),
            "gpu": g[0], "vram_used": g[1], "vram_total": g[2], "load": os.getloadavg()[0],
            "ram_used": round((mem["MemTotal"] - mem["MemAvailable"]) / 2**20, 1),
            "ram_total": round(mem["MemTotal"] / 2**20, 1), "model": model, "worker": worker, "r_drafted": len(drafted), "r_size": max(r_size, len(drafted)), "cur_try": cur_try,
            "r_pass": sum(" PASS " in ln for ln in rnd), "r_fail": sum("FAIL" in ln or "gave up" in ln for ln in rnd),
            "avg": avg, "eta": eta,
            "draft_name": str(draft.relative_to(ROOT)) if draft else "none yet",
            "draft": draft.read_text() if draft else ""}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps(data()).encode() if self.path == "/data" else PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json" if self.path == "/data" else "text/html")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


ThreadingHTTPServer(("127.0.0.1", 8765), H).serve_forever()
