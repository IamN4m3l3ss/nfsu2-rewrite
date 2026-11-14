#!/usr/bin/env bash
# M2 acceptance: launch + connect + screenshot + kill, N times in a row, unattended.
#   tests/m2_loop.sh [N]     exit code = number of failed runs
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
N="${1:-5}"
PY="$ROOT/.venv/bin/python"
fails=0
for i in $(seq "$N"); do
  t0=$(date +%s%3N)
  if ! out=$("$ROOT/tools/game/run_game.sh" 2>&1); then
    echo "run $i: FAIL launch: $(echo "$out" | tail -1)"; fails=$((fails + 1))
    "$ROOT/tools/game/run_game.sh" --kill; continue
  fi
  t_ready=$(date +%s%3N)
  shot=$("$PY" "$ROOT/tools/probe/probe.py" shot-wait 4 30 2>&1)
  shot_rc=$?
  path=${shot%% *}
  "$ROOT/tools/game/run_game.sh" --kill
  kill_rc=$?
  if [[ $shot_rc -ne 0 || ! -s "$path" ]]; then
    echo "run $i: FAIL screenshot: $shot"; fails=$((fails + 1)); continue
  fi
  if [[ $kill_rc -ne 0 ]]; then
    echo "run $i: FAIL kill"; fails=$((fails + 1)); continue
  fi
  echo "run $i: OK ready in $((t_ready - t0)) ms, shot $path"
done
echo "$fails failure(s) in $N runs"
exit "$fails"
