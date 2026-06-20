#!/bin/zsh
# perf_capture.sh [--dur N] [--res WxH] [--scenario NAME] [--gpu] [--idle] [--port P]
#
# One command = a clean 1440p frame-budget MEASUREMENT run + a verdict. Boots the
# server, runs the UE client headless-ish (-game windowed, no human), lets the
# in-engine frame accountant (ASeaWorldManager) sample steady-state, then prints
# the verdict via perf_summary.sh. This is the canonical "did the last change
# blow the frame budget?" check (runbook §5).
#
# Defaults measure the WORST realistic case: game-mode wave load at 2560x1440.
#   --gpu    also dump a per-pass GPU breakdown (ProfileGPU) into the log so the
#            cost can be attributed (water vs Lumen vs cloud) without an overlay.
#   --idle   observer seat, no auto-gunner — isolates the static ocean/sky cost.
#   --dur    SeaQuit seconds measured from world begin (default 30; needs >~12 to
#            clear the 8 s warmup and log >60 steady frames).
#
# Measurement hygiene (runbook §5): NO screenshot (its ~400 ms GPU readback would
# poison max/avg) — we use -SeaQuit. vsync stays ON (shipping config; r.VSync=0 is
# a slower present path on Metal windowed, invalid for headroom). The verdict
# therefore keys on over-budget%/p99, not the vsync-pinned avg.
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
UE="${UE:-/Users/Shared/Epic Games/UE_5.8}"
PROJ="$ROOT/client/SeaShield/SeaShield.uproject"

DUR=30; RESX=2560; RESY=1440; SCN=game.scn; GPU=0; IDLE=0; PORT=7777; CVARS=""; CINE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dur) DUR=$2; shift 2;;
    --res) RESX=${2%x*}; RESY=${2#*x}; shift 2;;
    --scenario) SCN=$2; shift 2;;
    --gpu) GPU=1; shift;;
    --idle) IDLE=1; shift;;
    --port) PORT=$2; shift 2;;
    --cvars) CVARS=$2; shift 2;;   # comma-list to -dpcvars, e.g. r.Water.WaterMesh.TessFactorBias=-6
    --cinematic) CINE=1; shift;;   # apply the cinematic capture tier (Tools/cinematic.cvars) — fps unconstrained
    *) echo "perf_capture: unknown arg $1" >&2; exit 2;;
  esac
done

# The cinematic tier prepends Tools/cinematic.cvars (comments/blanks stripped,
# joined with commas) to any explicit --cvars, so --cvars still wins on conflict
# (-dpcvars takes the LAST value for a given cvar). See cinematic.cvars header.
if [ "$CINE" = 1 ]; then
  CINE_FILE="$(dirname "$0")/cinematic.cvars"
  [ -f "$CINE_FILE" ] || { echo "perf_capture: $CINE_FILE missing" >&2; exit 1; }
  CINE_LIST="$(grep -v '^[[:space:]]*#' "$CINE_FILE" | grep -v '^[[:space:]]*$' | tr -d '[:blank:]' | paste -sd, -)"
  [ -n "$CVARS" ] && CVARS="$CINE_LIST,$CVARS" || CVARS="$CINE_LIST"
fi

[ -x "$ROOT/build/seashield_server" ] || {
  echo "server binary missing — build it first:  cmake --build build" >&2; exit 1; }
[ "$DUR" -ge 12 ] || echo "perf_capture: warning — --dur $DUR < 12 s may log no steady frames" >&2

ABSLOG="/tmp/seashield_perf_$PORT.log"; : > "$ABSLOG"
MARKER="SeaServer=127.0.0.1:$PORT"

# Server: session-detached so the editor's teardown/watchdog can't take it down,
# and so cleanup is by THIS port only (never broad pkill — that kills a
# concurrent capture's server). game.scn is endless, so the stream lasts the run.
python3 -c "import os,sys; os.setsid(); os.execv(sys.argv[1], sys.argv[1:])" \
  "$ROOT/build/seashield_server" --scenario "$ROOT/scenarios/$SCN" \
  --port "$PORT" --udp-port "$((PORT + 1))" >/tmp/seashield_perf_srv_$PORT.log 2>&1 &
SRV=$!
sleep 1.5

FLAGS=(-game -windowed "-ResX=$RESX" "-ResY=$RESY" -nosplash "-SeaServer=127.0.0.1:$PORT" "-SeaQuit=$DUR")
if [ "$IDLE" = 1 ]; then FLAGS+=(-SeaRole=observer); else FLAGS+=(-SeaRole=solo -SeaGamePlay); fi
[ "$GPU" = 1 ] && FLAGS+=("-SeaProfileGPU=$((DUR - 4))")
[ "$CINE" = 1 ] && FLAGS+=("-SeaCinematic")
[ -n "$CVARS" ] && FLAGS+=("-dpcvars=$CVARS")
FLAGS+=("-abslog=$ABSLOG")

echo "perf_capture: ${RESX}x${RESY}  dur=${DUR}s  scenario=$SCN  $([ "$IDLE" = 1 ] && echo idle || echo gameplay)$([ "$GPU" = 1 ] && echo ' +gpu')$([ "$CINE" = 1 ] && echo ' +cinematic')"
"$UE/Engine/Binaries/Mac/UnrealEditor" "$PROJ" "${FLAGS[@]}" >/tmp/seashield_perf_ue_$PORT.log 2>&1 &

# Monitor by COMMAND LINE, not PID: the Mac launcher exits early while the real
# editor keeps running (runbook §8). Wait for it to appear, then to vanish, with
# a stall watchdog on the log and an absolute timeout.
cleanup() {
  pkill -f "UnrealEditor.*$MARKER" 2>/dev/null
  pkill -f "seashield_server.*--port $PORT( |\$)" 2>/dev/null
  kill "$SRV" 2>/dev/null
}
trap cleanup EXIT INT TERM

DEADLINE=$(( $(date +%s) + DUR + 120 ))   # boot(~10s) + DUR + teardown margin
seen=0; last=0; quiet=0
while :; do
  sleep 3
  if pgrep -f "$MARKER" >/dev/null 2>&1; then
    seen=1
    cur=$(stat -f%z "$ABSLOG" 2>/dev/null || echo 0)
    if [ "$cur" = "$last" ]; then quiet=$((quiet + 3)); else quiet=0; last=$cur; fi
    if [ "$quiet" -ge 90 ]; then echo "perf_capture: STALL — editor log idle 90 s, killing" >&2; break; fi
  elif [ "$seen" = 1 ]; then
    break   # editor booted and has now exited cleanly (SeaQuit)
  fi
  [ "$(date +%s)" -ge "$DEADLINE" ] && { echo "perf_capture: TIMEOUT, killing" >&2; break; }
done

cleanup; trap - EXIT INT TERM
sleep 1
echo
"$(dirname "$0")/perf_summary.sh" "$ABSLOG"
