#!/usr/bin/env python3
"""Live page for the local-model loop: http://127.0.0.1:8765
Progress, workers, failure reasons (click to filter), per-function history with the code of every try, benchmark
results, log, GPU/CPU/RAM. Reads work/llm/status.json, log.txt, failed/, bench_*.json written by llm_batch.py."""
import glob
import json
import os
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / "work" / "llm"

# failure categories: (label, regex on the log text after the address)
CATS = [
    ("format: no code block", r"rejected \(format: no "),
    ("format: missing test", r"rejected \(format: missing"),
    ("format: calls game code", r"rejected \(format: calls"),
    ("format: inline asm", r"rejected \(format: inline"),
    ("format (old log)", r"rejected \(format\)"),
    ("too long", r"rejected \(too long\)"),
    ("compile: name clash", r"compile error: duplicate symbol|another file already defines"),
    ("compile: no matching helper call", r"compile error: no matching function"),
    ("compile: undeclared name", r"compile error: use of undeclared"),
    ("compile: type / conversion", r"compile error: .*(cannot initialize|no viable conversion|static_cast|reinterpret_cast|incompatible|indirection)"),
    ("compile: syntax", r"compile error: (expected|unexpected|too many arguments|too few arguments)"),
    ("compile: other", r"compile error"),
    ("test: test spec invalid", r"test FAIL: Spec:"),
    ("test: original crashed (spec missing a link)", r"test FAIL: exception C0000005 at 00[4-7]"),
    ("test: rewrite crashed", r"test FAIL: exception"),
    ("test: wrong stack cleanup (pops)", r"test FAIL: .*pops"),
    ("test: wrong return value (eax)", r"test FAIL: .*\beax\b"),
    ("test: wrong float result (st0)", r"test FAIL: .*st0"),
    ("test: wrong memory writes", r"test FAIL: .*mem\["),
    ("test: other", r"test FAIL"),
]

PAGE = r"""<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>LLM Loop</title>
<style>
:root{--bg:#0e1116;--panel:#161b22;--line:#2a313c;--text:#d7dde5;--dim:#8b95a3;--ok:#3fb950;--bad:#f85149;
--warn:#d29922;--acc:#58a6ff}
*{box-sizing:border-box}html,body{height:100%}
body{margin:0;background:var(--bg);color:var(--text);font:13px/1.4 system-ui,sans-serif;display:flex;flex-direction:column}
header{display:flex;align-items:center;gap:14px;padding:8px 16px;border-bottom:1px solid var(--line);flex-wrap:wrap}
header h1{font-size:16px;margin:0}.pill{padding:2px 10px;border-radius:99px;font-weight:600;font-size:12px}
.run{background:#1f6f3b}.idle{background:#444}.bench{background:#6e4b00}.dim{color:var(--dim)}
main{flex:1;display:grid;gap:10px;padding:10px 16px;min-height:0;
grid-template-columns:minmax(0,1.25fr) minmax(0,1fr);grid-template-rows:auto auto minmax(0,1fr)}
@media(max-width:1100px){main{grid-template-columns:1fr;grid-template-rows:none}}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:9px 12px;min-height:0;
display:flex;flex-direction:column}.panel h2{font-size:11px;text-transform:uppercase;letter-spacing:.06em;
color:var(--dim);margin:0 0 7px;display:flex;gap:8px;align-items:center}
.kpis{grid-column:1/-1;display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
.kpi .v{font-size:24px;font-weight:600}.kpi .s{color:var(--dim);font-size:12px}
.bar{height:6px;background:#2a313c;border-radius:3px;margin-top:6px;overflow:hidden;display:flex}.bar i{display:block;height:100%}
.w{display:grid;grid-template-columns:62px 80px 1fr;gap:8px;padding:4px 0;border-bottom:1px solid var(--line)}
.w:last-child{border:0}.st{font-weight:600}
.scroll{overflow:auto;min-height:0;flex:1}
table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:3px 6px;border-bottom:1px solid var(--line);
white-space:nowrap;vertical-align:top}th{color:var(--dim);font-weight:500;position:sticky;top:0;background:var(--panel)}
td.n{white-space:normal}tr.f{cursor:pointer}tr.f:hover{background:#1d2430}tr.sel{background:#22304a}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.acc{color:var(--acc)}
pre{margin:0;font:12px/1.45 ui-monospace,monospace;white-space:pre-wrap}
.sys{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
.reason{display:grid;grid-template-columns:1fr 34px;gap:2px 8px;align-items:center;cursor:pointer;padding:2px 4px;border-radius:4px}
.reason:hover{background:#1d2430}.reason.on{background:#22304a}.rb{height:5px;background:var(--bad);border-radius:3px;opacity:.7}
.chips{display:flex;gap:6px;flex-wrap:wrap}.chip{border:1px solid var(--line);border-radius:99px;padding:1px 9px;cursor:pointer;
font-size:12px;background:none;color:var(--text)}.chip.on{background:#22304a;border-color:var(--acc)}
.tabs button{background:none;border:0;color:var(--dim);padding:2px 8px;cursor:pointer;font:inherit}
.tabs button.on{color:var(--text);border-bottom:2px solid var(--acc)}
.hist{border-left:2px solid var(--line);padding-left:8px;margin:4px 0}.hist b{cursor:pointer}
.left{display:flex;flex-direction:column;gap:10px;min-height:0}.right{display:flex;flex-direction:column;gap:10px;min-height:0}
</style>
<header><h1>Local LLM rewrite loop</h1><span id=state class=pill>…</span><span id=model class=dim></span>
<span id=elapsed class=dim style="margin-left:auto"></span></header>
<main>
<div class=kpis id=kpis></div>
<div class=panel><h2>Workers + tester <span class=dim style="text-transform:none;letter-spacing:0">(drafting runs in parallel on the GPU; one game tests one batch at a time)</span></h2><div id=workers></div></div>
<div class=panel><h2>Your PC</h2><div class=sys id=sys></div></div>
<div class=left>
 <div class=panel style="flex:0 0 auto;max-height:34%"><h2>Why tries failed <span class=dim id=rtotal></span>
  <span class=dim style="margin-left:auto;text-transform:none;letter-spacing:0">click to filter the table</span></h2>
  <div class=scroll id=reasons></div></div>
 <div class=panel style="flex:1"><h2>Functions <span class=dim id=fcount></span><span class=chips id=sfilter style="margin-left:auto"></span></h2>
  <div class=scroll><table><thead><tr><th>Address</th><th>Function</th><th>Level</th><th>Tries</th><th>Status</th><th>Last result</th>
  </tr></thead><tbody id=funcs></tbody></table></div></div>
</div>
<div class=right>
 <div class=panel style="flex:1"><div class=tabs><button id=tF class=on>Function</button><button id=tD>Latest draft</button>
  <button id=tB>Benchmarks</button><button id=tL>Log</button><span class=dim id=vname></span></div>
  <div class=scroll id=vs style="margin-top:6px"><div id=view></div></div></div>
</div>
</main>
<script>
let tab='func',D=null,sel=null,fReason=null,fStatus='all',codeOf=null;const $=id=>document.getElementById(id);
for(const [b,t] of [['tF','func'],['tD','draft'],['tB','bench'],['tL','log']])$(b).onclick=()=>{tab=t;for(const x of ['tF','tD','tB','tL'])$(x).className=x==b?'on':'';draw()};
const esc=s=>String(s??'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
function kpi(t,v,s,b){return `<div class="panel kpi"><h2>${t}</h2><div class=v>${v}</div><div class=s>${s||''}</div>${b||''}</div>`}
function bar(parts){return '<div class=bar>'+parts.map(([p,c])=>`<i style="width:${p}%;background:${c}"></i>`).join('')+'</div>'}
function meter(t,v,p,s){return `<div><div class=dim>${t}</div><div style="font-size:17px;font-weight:600">${v}</div>
<div class=dim>${s||''}</div>${bar([[p,p>90?'var(--bad)':p>70?'var(--warn)':'var(--acc)']])}</div>`}
function fmt(s){s=Math.round(s);return s<60?s+'s':s<3600?Math.floor(s/60)+'m '+s%60+'s':Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m'}
const SC={passed:'ok','gave up':'bad',drafting:'acc',testing:'acc',retry:'warn',queued:'dim'};
async function showCode(key,tr){const r=await (await fetch(`/code?key=${key}&try=${tr}`)).json();codeOf={key,tr,text:r.text};tab='func';draw()}
window.showCode=showCode;window.pick=k=>{sel=k;codeOf=null;tab='func';for(const x of ['tF','tD','tB','tL'])$(x).className=x=='tF'?'on':'';draw()};
window.fr=c=>{fReason=fReason==c?null:c;draw()};window.fs=s=>{fStatus=s;draw()};
function draw(){const d=D;if(!d)return;const r=d.run;
const tot=r.total||0,pa=r.passed||0,ga=r.gave_up||0,fin=pa+ga,act=d.active,fd=r.first_drafts||0;
$('state').textContent=act?(r.bench?'BENCHMARK running':'running'):'idle';$('state').className='pill '+(act?(r.bench?'bench':'run'):'idle');
$('model').textContent=r.model?('model: '+r.model+(r.bench?'  ·  bench '+r.bench:'')):'';
$('elapsed').textContent=r.elapsed_s?((act?'running for ':'ran for ')+fmt(r.elapsed_s)):'';
const eta=(act&&fd>=5&&r.drafts)?fmt(Math.max(0,tot*r.drafts/fd-r.drafts)*r.elapsed_s/r.drafts):(act?'estimating':'–');
$('kpis').innerHTML=kpi('Progress',`${fin} / ${tot}`,`finished · first drafts ${fd}/${tot}`,bar([[tot?100*pa/tot:0,'var(--ok)'],[tot?100*ga/tot:0,'var(--bad)']]))
+kpi('Time left',eta,'from the draft rate so far')
+kpi('Passed',`<span class=ok>${pa}</span>`,fin?`${Math.round(100*pa/fin)}% of finished`:'')
+kpi('Gave up',`<span class=bad>${ga}</span>`,`after ${d.tries} tries`)
+kpi('Passes / hour',r.passes_per_hour||0,pa?`avg ${(r.tries_of_passes/pa).toFixed(1)} tries per pass`:'')
+kpi('Drafts',r.drafts||0,`waiting: ${r.draft_queue||0} new, ${r.retry_queue||0} retries, ${r.test_queue||0} to test`)
+kpi('Speed',`${r.tok_s||0}`,'tokens/s per request');
let w='';const ws=r.workers||{};
for(const k of Object.keys(ws).sort()){const v=ws[k];const c=v.startsWith('drafting')?'acc':v=='done'?'dim':'warn';
w+=`<div class=w><span>worker ${k}</span><span class="st ${c}">${v.split(' ')[0]}</span><span>${esc(v.split(' ').slice(1).join(' '))}</span></div>`}
const t=r.tester||'idle';w+=`<div class=w><span>tester</span><span class="st ${t=='idle'||t=='done'?'dim':'ok'}">${t.split(' ')[0]}</span><span>${esc(t.split(' ').slice(1).join(' '))}</span></div>`;
$('workers').innerHTML=w;
const g=d.sys;$('sys').innerHTML=meter('GPU',g.gpu+'%',g.gpu)+meter('VRAM',g.vram_used+' GB',100*g.vram_used/(g.vram_total||1),'of '+g.vram_total+' GB')
+meter('CPU load',g.load.toFixed(1),100*g.load/g.cpus,g.cpus+' threads')+meter('RAM',g.ram_used+' GB',100*g.ram_used/g.ram_total,'of '+g.ram_total+' GB');
const rs=Object.entries(d.reasons).sort((a,b)=>b[1]-a[1]),rmax=rs.length?rs[0][1]:1,rt=rs.reduce((a,b)=>a+b[1],0);
$('rtotal').textContent=`(${rt} failed tries)`;
$('reasons').innerHTML=rs.map(([c,n])=>`<div class="reason ${fReason==c?'on':''}" onclick='fr(${JSON.stringify(c)})'><div>${esc(c)}
<div class=rb style="width:${100*n/rmax}%"></div></div><div style="text-align:right">${n}</div></div>`).join('')||'<span class=dim>none yet</span>';
const sts=['all','drafting','testing','retry','passed','gave up'];
$('sfilter').innerHTML=sts.map(s=>`<button class="chip ${fStatus==s?'on':''}" onclick='fs(${JSON.stringify(s)})'>${s}</button>`).join('')
+(fReason?`<button class="chip on" onclick="fr(null)">reason: ${esc(fReason)} ✕</button>`:'');
const order={drafting:0,testing:1,retry:2,passed:3,'gave up':4,queued:5};
let fs_=d.funcs.filter(f=>(fStatus=='all'||f.status==fStatus)&&(!fReason||f.history.some(h=>h.cat==fReason)));
fs_.sort((a,b)=>(order[a.status]??9)-(order[b.status]??9));
$('fcount').textContent=`(${fs_.length} of ${d.funcs.length})`;
$('funcs').innerHTML=fs_.map(f=>`<tr class="f ${sel==f.addr?'sel':''}" onclick="pick('${f.addr}')"><td>${f.addr}</td><td class=n>${esc(f.name)}</td>
<td>${f.level}</td><td>${f.tries}</td><td class=${SC[f.status]||'dim'}>${f.status}</td><td class=n>${esc(f.last)}</td></tr>`).join('');
const v=$('view'),sc=$('vs');
if(tab=='func'){const f=d.funcs.find(x=>x.addr==sel);
 if(!f){v.innerHTML='<span class=dim>Click a function in the table to see every try: why it failed, and the code it wrote.</span>';$('vname').textContent=''}
 else{$('vname').textContent='  '+f.addr+' '+f.name;
  v.innerHTML=`<div class=dim>level ${f.level} · ${f.tries} tries · <span class=${SC[f.status]}>${f.status}</span></div>`+
  f.history.map(h=>`<div class=hist><span class=dim>${h.t}</span> <b class=${h.kind=='pass'?'ok':h.kind=='draft'?'acc':'bad'}>${esc(h.cat||h.kind)}</b>
  ${h.code?` · <a href="#" onclick="showCode('${f.addr}',${h.try});return false" class=acc>code of try ${h.try}</a>`:''}<div class=dim>${esc(h.text)}</div></div>`).join('')+
  (codeOf&&codeOf.key==f.addr?`<h2 style="margin-top:10px">Try ${codeOf.tr}</h2><pre>${esc(codeOf.text)}</pre>`:'')}}
else if(tab=='draft'){$('vname').textContent='  '+d.draft_name;v.innerHTML=`<pre>${esc(d.draft)}</pre>`}
else if(tab=='bench'){$('vname').textContent='';v.innerHTML='<table><thead><tr><th>Run</th><th>Passed</th><th>Passes/h</th><th>Tries/pass</th><th>tok/s</th><th>VRAM peak</th><th>Time</th></tr></thead><tbody>'+
 d.bench.map(b=>`<tr><td class=n>${esc(b.tag)}</td><td>${b.passed} / ${b.total}</td><td>${b.pph}</td><td>${b.tpp}</td><td>${b.tok_s}</td><td>${b.vram}</td><td>${b.time}</td></tr>`).join('')+'</tbody></table>'}
else{$('vname').textContent='';const atEnd=sc.scrollTop+sc.clientHeight>=sc.scrollHeight-20;v.innerHTML=`<pre>${esc(d.log)}</pre>`;if(atEnd)sc.scrollTop=1e9}}
async function tick(){try{D=await (await fetch('/data')).json();draw()}catch(e){}}
tick();setInterval(tick,2000)</script>"""

_levels = {}


def level(addr):
    """low / medium / high from the machine code (same grading as the breakdown)"""
    if addr in _levels:
        return _levels[addr]
    try:
        import sys
        sys.path.insert(0, str(ROOT / "tools/re"))
        import capstone
        import kb
        import xrefs
        if not hasattr(level, "ctx"):
            idx = sorted(kb.dump_index("pc"))
            level.ctx = (xrefs.pe(), capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32),
                         {a: idx[i + 1] for i, a in enumerate(idx[:-1])})
        pe, md, nxt = level.ctx
        a = int(addr, 16)
        ins = [x for x in md.disasm(pe.get_data(a - 0x400000, min(nxt.get(a, a + 0x80) - a, 0x400)), a)
               if x.mnemonic not in ("nop", "int3")]
        br = sum(x.mnemonic.startswith("j") and x.mnemonic != "jmp" for x in ins)
        fp = sum(x.mnemonic.startswith("f") for x in ins)
        fc = sum(x.mnemonic in ("fnstsw", "fcomp", "fcom", "fcompp", "fucomp") for x in ins)
        loop = any(x.mnemonic.startswith("j") and x.op_str.startswith("0x") and int(x.op_str, 16) < x.address for x in ins)
        s = len(ins) + 3 * br + 2 * fc + (8 if loop else 0) + (3 if fp else 0)
        _levels[addr] = "low" if s <= 18 else "medium" if s <= 40 else "high"
    except Exception:  # noqa: BLE001
        _levels[addr] = "?"
    return _levels[addr]


def cat(text):
    for label, rx in CATS:
        if re.search(rx, text):
            return label
    return ""


def gpu():
    for dev in glob.glob("/sys/class/drm/card*/device"):
        try:
            tot = int(open(f"{dev}/mem_info_vram_total").read()) / 2**30
            if tot > 4:
                return (int(open(f"{dev}/gpu_busy_percent").read()),
                        round(int(open(f"{dev}/mem_info_vram_used").read()) / 2**30, 1), round(tot, 1))
        except OSError:
            pass
    return 0, 0, 0


def funcs(lines):
    """per-function state + history from the log of the current run (since the last 'start:')"""
    k = max([i for i, ln in enumerate(lines) if " start: " in ln] or [0])
    out, reasons = {}, {}
    failed = {Path(p).stem for p in glob.glob(str(WORK / "failed" / "*.cpp"))}
    for ln in lines[k:]:
        p = ln.split()
        if len(p) < 3 or not re.fullmatch(r"[0-9A-F]{6}", p[1]):
            continue
        f = out.setdefault(p[1], {"addr": p[1], "name": "", "tries": 0, "status": "queued", "last": "", "history": []})
        rest = " ".join(p[2:])
        ev = {"t": p[0], "text": rest, "kind": "", "cat": "", "try": f["tries"], "code": False}
        if p[2] == "draft":
            f["tries"] = int(p[3]) if p[3].isdigit() else f["tries"]
            ev["try"] = f["tries"]
            if "rejected" in rest:
                ev["kind"], ev["cat"] = "fail", cat(rest)
                f["status"], f["last"] = "retry", ev["cat"]
                ev["code"] = f"{p[1]}_try{f['tries']}" in failed
            else:
                ev["kind"] = "draft"
                f["status"], f["last"] = "testing", f"draft {p[3]} written in {p[4]}"
                m = re.search(r" w\d+ (.*)$", rest)
                f["name"] = m.group(1) if m else f["name"]
        elif p[2] == "PASS":
            ev["kind"] = "pass"
            f["status"], f["last"] = "passed", rest
        elif "compile error" in rest or p[2] == "test":
            ev["kind"], ev["cat"] = "fail", cat(rest)
            ev["code"] = f"{p[1]}_try{f['tries']}" in failed
            f["status"], f["last"] = "retry", (ev["cat"] + ": " + rest.split(": ", 1)[-1])[:160]
        elif p[2] == "gave":
            ev["kind"] = "gave up"
            f["status"] = "gave up"
        else:
            ev["kind"] = "info"
        if ev["cat"]:
            reasons[ev["cat"]] = reasons.get(ev["cat"], 0) + 1
        f["history"].append(ev)
    for f in out.values():
        f["level"] = level(f["addr"])
    return out, reasons


def bench():
    rows = []
    for p in sorted(glob.glob(str(WORK / "bench_*.json")), key=os.path.getmtime):
        try:
            d = json.load(open(p))
        except Exception:  # noqa: BLE001
            continue
        pa = d.get("passed", 0)
        rows.append({"tag": Path(p).stem[6:], "passed": pa, "total": d.get("total", 47), "pph": d.get("passes_per_hour", 0),
                     "tpp": round(d.get("tries_of_passes", 0) / pa, 1) if pa else "–", "tok_s": d.get("tok_s", 0),
                     "vram": f"{d['vram_peak_mb'] / 1024:.1f} GB" if d.get("vram_peak_mb") else "–",
                     "time": f"{d.get('elapsed_s', 0) // 60} min"})
    return rows


def data():
    log = (WORK / "log.txt").read_text().splitlines() if (WORK / "log.txt").exists() else []
    sf = WORK / "status.json"
    run = json.loads(sf.read_text()) if sf.exists() else {}
    fs, reasons = funcs(log)
    for wv in run.get("workers", {}).values():  # who is drafting what right now
        m = re.match(r"drafting ([0-9A-F]{6}) (.*) \(try (\d+)\)", wv)
        if m:
            f = fs.setdefault(m.group(1), {"addr": m.group(1), "name": "", "tries": 0, "status": "", "last": "",
                                           "history": [], "level": level(m.group(1))})
            f.update(status="drafting", name=m.group(2), tries=int(m.group(3)))
    pid = run.get("pid")
    active = os.path.exists(f"/proc/{pid}") if pid else (sf.exists() and os.path.getmtime(sf) > time.time() - 30)
    mem = {k: int(v.split()[0]) for k, v in (ln.split(":", 1) for ln in open("/proc/meminfo"))}
    g = gpu()
    drafts = sorted(glob.glob(str(ROOT / "game/systems/*/llm/f_*.cpp")), key=os.path.getmtime)
    draft = Path(drafts[-1]) if drafts else None
    return {"run": run, "active": active, "tries": run.get("tries", 3), "funcs": list(fs.values()), "reasons": reasons,
            "bench": bench(), "log": "\n".join(log[-400:]),
            "draft_name": str(draft.relative_to(ROOT)) if draft else "no draft on disk (bench drafts are deleted after the test)",
            "draft": draft.read_text() if draft else "",
            "sys": {"gpu": g[0], "vram_used": g[1], "vram_total": g[2], "load": os.getloadavg()[0],
                    "cpus": os.cpu_count(), "ram_used": round((mem["MemTotal"] - mem["MemAvailable"]) / 2**20, 1),
                    "ram_total": round(mem["MemTotal"] / 2**20, 1)}}


def code(q):
    key = re.sub(r"[^0-9A-F]", "", q.get("key", [""])[0])[:6]
    tr = re.sub(r"\D", "", q.get("try", ["0"])[0])[:2]
    p = WORK / "failed" / f"{key}_try{tr}.cpp"
    if p.exists():
        return {"text": p.read_text()}
    live = glob.glob(str(ROOT / f"game/systems/*/llm/f_{key}.cpp"))
    return {"text": Path(live[0]).read_text() if live else "(no code kept for this try)"}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/data":
            body, ct = json.dumps(data()).encode(), "application/json"
        elif u.path == "/code":
            body, ct = json.dumps(code(parse_qs(u.query))).encode(), "application/json"
        else:
            body, ct = PAGE.encode(), "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ct)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


ThreadingHTTPServer(("127.0.0.1", 8765), H).serve_forever()
