#!/usr/bin/env bash
# Benchmark the local-model candidates one after another on the same function list (llm_batch.py --bench).
# Per model: start llama-server (Vulkan, all layers on the GPU unless noted), sample peak VRAM, run, stop.
# Results: work/llm/bench_<tag>.json (+ vram_peak_mb). Needs the game + probe running.
set -u
cd "$(dirname "$0")/../../.."
G=${GGUF_DIR:-/mnt/m2drive/llm/gguf}
LIST=${1:-work/llm/bench50.txt}
# tag | gguf | slots | total ctx | extra server args | extra llm_batch args
CONFIGS=(
  "mellum21-12b-a2.5b-q6-np4|Mellum2.1-12B-A2.5B-Thinking-Q6_K.gguf|4|49152||--max-tokens 6000"
  "devstral2-24b-iq4xs-np1|Devstral-Small-2-24B-Instruct-2512-IQ4_XS.gguf|1|12288|"
  "qwen36-35b-a3b-q4xl-np4-cpumoe|Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf|4|49152|--n-cpu-moe 22"
)
while pgrep -f 'llm_batch[.]py' >/dev/null; do sleep 20; done  # a run in progress keeps the game
# the game is only a test bed: every reload of the rewrite DLL would reset live rewrites' state (broken UI/audio)
./nfsu2 all-off >/dev/null
for c in "${CONFIGS[@]}"; do
  IFS='|' read -r tag file np ctx extra batch <<<"$c"
  pkill -x llama-server; sleep 3
  echo "$(date +%T) $tag: starting server"
  # shellcheck disable=SC2086
  setsid llama-server -m "$G/$file" -ngl 999 -c "$ctx" -np "$np" -ctk q8_0 -ctv q8_0 -fa on $extra \
    --host 127.0.0.1 --port 8080 > "work/llm/server_$tag.log" 2>&1 &
  for _ in $(seq 120); do curl -s 127.0.0.1:8080/health | grep -q ok && break; sleep 2; done
  if ! curl -s 127.0.0.1:8080/health | grep -q ok; then echo "$(date +%T) $tag: server failed"; continue; fi
  peak=0
  ( while :; do v=$(( $(cat /sys/class/drm/card1/device/mem_info_vram_used) / 1048576 )); echo "$v"; sleep 2; done ) \
    > "work/llm/vram_$tag.txt" & sampler=$!
  .venv/bin/python tools/re/llm/llm_batch.py "$LIST" --max 50 --workers "$np" --api http://127.0.0.1:8080/v1 \
    --model "$tag" --bench "$tag" $batch >> work/llm/bench_all.out 2>&1
  kill $sampler
  peak=$(sort -n "work/llm/vram_$tag.txt" | tail -1)
  .venv/bin/python - "$tag" "$peak" <<'PY'
import json, sys
p = f"work/llm/bench_{sys.argv[1]}.json"
d = json.load(open(p)); d["vram_peak_mb"] = int(sys.argv[2]); json.dump(d, open(p, "w"), indent=1)
PY
  echo "$(date +%T) $tag: done, VRAM peak ${peak} MB"
done
pkill -x llama-server
./nfsu2 all-on >/dev/null
echo "$(date +%T) all benchmarks done (rewrites back on)"
