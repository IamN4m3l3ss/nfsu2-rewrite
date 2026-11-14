#!/usr/bin/env bash
# Launch / stop NFSU2 under the project's Wine prefix.
#
#   tools/run_game.sh [--freeroam] [--no-traffic] [--desktop WxH] [--wait] [--no-probe] [--timeout SEC]
#   tools/run_game.sh --kill
#
# By default returns once the probe answers and the game has presented frames ("ready").
# --no-traffic: probe stops traffic from startup (repeatable drives).
# --freeroam then drives the menus into Explore Mode with injected keys (tools/boot_freeroam.py, needs a save).
#
# The game runs from work/profile: a symlink copy of GAME_DIR, so the user's folder is never written to.
# Every ASI in GAME_DIR/scripts/ is linked in: the user's mod set is REQUIRED to run the game properly
# (WidescreenFix = no-disc fix, HDContentSupport = HD UI textures in the data). Our probe, when built,
# is copied next to them.
#
# Exit codes: 0 ok, 2 bad args/config, 3 exe hash mismatch, 4 game did not start, 5 probe/frames not ready,
#             6 game survived --kill, 7 --freeroam failed. A game that is already running is killed first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/.env.local"
export WINEPREFIX
WINE="${WINE:-wine}"
EXPECTED_SHA256="f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7"
PROFILE_DIR="$ROOT/work/profile"
LOG_DIR="$ROOT/work/logs"

desktop=""
wait_exit=0
wait_probe=1
freeroam=0
probe_timeout=60

kill_game() {
  wineserver -k 2>/dev/null || true
  for _ in $(seq 40); do
    pgrep -f '\\SPEED2\.EXE' >/dev/null || return 0
    sleep 0.25
  done
  pkill -9 -f '\\SPEED2\.EXE' || true
  sleep 0.5
  ! pgrep -f '\\SPEED2\.EXE' >/dev/null
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --desktop) desktop="$2"; shift 2 ;;
    --wait) wait_exit=1; shift ;;
    --no-probe) wait_probe=0; shift ;;
    --freeroam) freeroam=1; shift ;;
    --no-traffic) export NFSU2_NO_TRAFFIC=1; shift ;;
    --timeout) probe_timeout="$2"; shift 2 ;;
    --kill)
      kill_game || { echo "game still running after kill" >&2; exit 6; }
      exit 0 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -f "$GAME_DIR/SPEED2.EXE" ]] || { echo "GAME_DIR has no SPEED2.EXE: $GAME_DIR" >&2; exit 2; }
sha="$(sha256sum "$GAME_DIR/SPEED2.EXE" | cut -d' ' -f1)"
[[ "$sha" == "$EXPECTED_SHA256" ]] || { echo "SPEED2.EXE hash mismatch: $sha" >&2; exit 3; }

build_profile() {
  mkdir -p "$PROFILE_DIR/scripts"
  # refresh links (GAME_DIR contents may change)
  find "$PROFILE_DIR" -maxdepth 2 -type l -delete
  local entry name
  for entry in "$GAME_DIR"/*; do
    name="$(basename "$entry")"
    [[ "$name" == scripts ]] && continue
    ln -s "$entry" "$PROFILE_DIR/$name"
  done
  for entry in "$GAME_DIR"/scripts/*; do
    ln -s "$entry" "$PROFILE_DIR/scripts/$(basename "$entry")"
  done
  if [[ -f "$ROOT/out/nfsu2_probe.asi" ]]; then
    cp -f "$ROOT/out/nfsu2_probe.asi" "$PROFILE_DIR/scripts/"
  fi
}

if pgrep -f '\\SPEED2\.EXE' >/dev/null; then
  echo "game already running, killing it first"
  kill_game || { echo "could not kill running game" >&2; exit 6; }
fi
build_profile
mkdir -p "$LOG_DIR"
log="$LOG_DIR/game_$(date +%Y%m%d_%H%M%S).log"
ln -sfn "$log" "$LOG_DIR/game_latest.log"

export WINEDLLOVERRIDES="dinput8=n,b;d3d9=n,b"
# The loader (scripts/nfsu2_probe.asi) loads the core straight from the build output and reloads it whenever
# out/nfsu2_core.dll changes: rebuild while the game runs.
if [[ -f "$ROOT/out/nfsu2_core.dll" ]]; then
  NFSU2_CORE="$(winepath -w "$ROOT/out/nfsu2_core.dll" 2>/dev/null)"
  export NFSU2_CORE
fi
export WINEDEBUG="${WINEDEBUG:-err+all,fixme-all}"

cmd=("$WINE" SPEED2.EXE)
if [[ -n "$desktop" ]]; then
  # explorer resolves a bare name against its own cwd, so pass the full Windows path
  cmd=("$WINE" explorer "/desktop=nfsu2,$desktop" "$(winepath -w "$PROFILE_DIR/SPEED2.EXE" 2>/dev/null)")
fi

cd "$PROFILE_DIR"
echo "dir=$PROFILE_DIR log=$log"
"${cmd[@]}" >"$log" 2>&1 &
pid=$!

# wait up to 30 s for the game process to appear
for _ in $(seq 60); do
  if pgrep -f '\\SPEED2\.EXE' >/dev/null; then break; fi
  if ! kill -0 "$pid" 2>/dev/null; then echo "wine exited early, see $log" >&2; exit 4; fi
  sleep 0.5
done
pgrep -f '\\SPEED2\.EXE' >/dev/null || { echo "game did not start, see $log" >&2; exit 4; }
echo "started"

if [[ $wait_probe -eq 1 ]]; then
  # probe answers and the game has presented frames
  if ! "$ROOT/.venv/bin/python" "$ROOT/tools/probe.py" ready "$probe_timeout" >/dev/null; then
    echo "probe/game not ready within ${probe_timeout}s, see $log and work/profile/scripts/nfsu2_probe.log" >&2
    exit 5
  fi
  echo "ready"
fi

if [[ $freeroam -eq 1 ]]; then
  "$ROOT/.venv/bin/python" "$ROOT/tools/boot_freeroam.py" || exit 7
fi

if [[ $wait_exit -eq 1 ]]; then wait "$pid"; fi
