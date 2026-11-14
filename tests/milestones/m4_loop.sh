#!/usr/bin/env bash
# M4 acceptance: tools/game/run_game.sh --freeroam N times; each must reach free roam (< ~30 s). Exit = failures.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
N="${1:-5}"; fails=0
mkdir -p "$ROOT/work/logs/m4"
for i in $(seq "$N"); do
  t0=$(date +%s%3N)
  "$ROOT/tools/game/run_game.sh" --freeroam > "$ROOT/work/logs/m4/run_$i.txt" 2>&1
  rc=$?
  ms=$(( $(date +%s%3N) - t0 ))
  cp "$ROOT/work/profile/scripts/nfsu2_probe.log" "$ROOT/work/logs/m4/probe_$i.log" 2>/dev/null
  cp "$(readlink -f "$ROOT/work/logs/game_latest.log")" "$ROOT/work/logs/m4/wine_$i.log" 2>/dev/null
  if [[ $rc -eq 0 ]]; then echo "run $i: OK ${ms} ms total ($(tail -1 "$ROOT/work/logs/m4/run_$i.txt" | cut -d, -f1))"
  else echo "run $i: FAIL rc=$rc ${ms} ms: $(grep -m1 FAIL "$ROOT/work/logs/m4/run_$i.txt")"; grep -m1 'page fault' "$ROOT/work/logs/m4/wine_$i.log"; fails=$((fails+1)); fi
  "$ROOT/tools/game/run_game.sh" --kill
done
echo "$fails failure(s) in $N runs"; exit "$fails"
