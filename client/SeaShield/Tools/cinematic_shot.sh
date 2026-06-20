#!/bin/zsh
# cinematic_shot.sh [--seq I] [--shot T] [--dur N] [--res WxH] [--sp PCT] [--scenario NAME] [--port P]
#
# The TRAILER / SCREENSHOT path: boots the server + a UE client at the CINEMATIC
# capture tier (Tools/cinematic.cvars — full water tessellation, full-res
# reflections/clouds, Lumen reflections, native+supersampled internal res) and
# writes stills. fps is UNCONSTRAINED here — this tier exists so the marketing
# shots look AAA while the gameplay tier (perf_capture.sh, no --cinematic) keeps
# the 50/60 fps budget. Mirrors perf_capture.sh's session-detached server +
# command-line-matched monitor/cleanup (runbook §5/§8).
#
#   --seq I    filmstrip: a screenshot every I s of the live gunnery loop
#              (-SeaShotSeq, default mode, I=3).
#   --shot T   a single hero still at T s from the standard quarter view
#              (-SeaShot) instead of a filmstrip.
#   --sp PCT   supersample: internal render scale PCT% (overrides the tier's
#              100, e.g. 150 for print-grade stills). Cinematic only — costs fps.
#   --dur N    total run seconds (default 45; the run quits after).
#   --idle     observer seat (static sea/sky beauty), no auto-gunner.
#
# Output: client/SeaShield/Saved/Screenshots/MacEditor/SeaSeqNN.png (or SeaShot.png).
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
UE="${UE:-/Users/Shared/Epic Games/UE_5.8}"
PROJ="$ROOT/client/SeaShield/SeaShield.uproject"
SHOTDIR="$ROOT/client/SeaShield/Saved/Screenshots/MacEditor"

DUR=45; RESX=2560; RESY=1440; SCN=game.scn; PORT=7779; SP=0; IDLE=0; AAR=0
MODE=seq; SEQ=3; SHOT=0; CAM=""; MAP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --seq) MODE=seq; SEQ=$2; shift 2;;
    --shot) MODE=shot; SHOT=$2; shift 2;;
    --cam) CAM=$2; shift 2;;       # hero framing "X,Y,Z,Pitch,Yaw" (stage coords) for --shot
    --map) MAP=$2; shift 2;;       # load a non-default map (e.g. /Game/SeaShield/Maps/L_RangeProbe)
    --dur) DUR=$2; shift 2;;
    --res) RESX=${2%x*}; RESY=${2#*x}; shift 2;;
    --sp) SP=$2; shift 2;;
    --scenario) SCN=$2; shift 2;;
    --idle) IDLE=1; shift;;
    --aar) AAR=1; shift;;          # force the After-Action Review card on screen (-SeaShowAAR)
    --port) PORT=$2; shift 2;;
    *) echo "cinematic_shot: unknown arg $1" >&2; exit 2;;
  esac
done

[ -x "$ROOT/build/seashield_server" ] || {
  echo "server binary missing — build it first:  cmake --build build" >&2; exit 1; }

# Cinematic cvar profile (comments/blanks stripped, comma-joined). A --sp
# override is appended LAST so it wins on r.ScreenPercentage (-dpcvars keeps the
# last value for a cvar).
CINE_FILE="$(dirname "$0")/cinematic.cvars"
[ -f "$CINE_FILE" ] || { echo "cinematic_shot: $CINE_FILE missing" >&2; exit 1; }
CVARS="$(grep -v '^[[:space:]]*#' "$CINE_FILE" | grep -v '^[[:space:]]*$' | tr -d '[:blank:]' | paste -sd, -)"
[ "$SP" != 0 ] && CVARS="$CVARS,r.ScreenPercentage=$SP"

ABSLOG="/tmp/seashield_cine_$PORT.log"; : > "$ABSLOG"
MARKER="SeaServer=127.0.0.1:$PORT"

python3 -c "import os,sys; os.setsid(); os.execv(sys.argv[1], sys.argv[1:])" \
  "$ROOT/build/seashield_server" --scenario "$ROOT/scenarios/$SCN" \
  --port "$PORT" --udp-port "$((PORT + 1))" >/tmp/seashield_cine_srv_$PORT.log 2>&1 &
SRV=$!
sleep 1.5

FLAGS=(-game -windowed "-ResX=$RESX" "-ResY=$RESY" -nosplash "-SeaServer=127.0.0.1:$PORT" -SeaCinematic "-dpcvars=$CVARS")
if [ "$IDLE" = 1 ]; then FLAGS+=(-SeaRole=observer); else FLAGS+=(-SeaRole=solo -SeaGamePlay); fi
[ "$AAR" = 1 ] && FLAGS+=(-SeaShowAAR)   # overlay the AAR card (needs an engagement: Launches>0)
if [ "$MODE" = seq ]; then
  FLAGS+=("-SeaShotSeq=$SEQ" "-SeaShotSeqQuit=$DUR")
else
  FLAGS+=("-SeaShot=$SHOT")   # spawns the quarter cam, shoots once, self-quits ~T+3s
  if [ -n "$CAM" ]; then      # hero framing override "X,Y,Z,Pitch,Yaw"
    IFS=, read -r cx cy cz cp cyaw <<< "$CAM"
    [ -n "$cx" ]   && FLAGS+=("-SeaShotX=$cx")
    [ -n "$cy" ]   && FLAGS+=("-SeaShotY=$cy")
    [ -n "$cz" ]   && FLAGS+=("-SeaShotZ=$cz")
    [ -n "$cp" ]   && FLAGS+=("-SeaShotPitch=$cp")
    [ -n "$cyaw" ] && FLAGS+=("-SeaShotYaw=$cyaw")
  fi
fi
FLAGS+=("-abslog=$ABSLOG")

echo "cinematic_shot: ${RESX}x${RESY}${SP:+ sp=$SP%}  dur=${DUR}s  scenario=$SCN  mode=$MODE  -> $SHOTDIR"
"$UE/Engine/Binaries/Mac/UnrealEditor" "$PROJ" ${MAP:+"$MAP"} "${FLAGS[@]}" >/tmp/seashield_cine_ue_$PORT.log 2>&1 &

cleanup() {
  pkill -f "UnrealEditor.*$MARKER" 2>/dev/null
  pkill -f "seashield_server.*--port $PORT( |\$)" 2>/dev/null
  kill "$SRV" 2>/dev/null
}
trap cleanup EXIT INT TERM

DEADLINE=$(( $(date +%s) + DUR + 150 ))   # boot + DUR + screenshot readbacks + teardown
seen=0; last=0; quiet=0
while :; do
  sleep 3
  if pgrep -f "$MARKER" >/dev/null 2>&1; then
    seen=1
    cur=$(stat -f%z "$ABSLOG" 2>/dev/null || echo 0)
    if [ "$cur" = "$last" ]; then quiet=$((quiet + 3)); else quiet=0; last=$cur; fi
    if [ "$quiet" -ge 120 ]; then echo "cinematic_shot: STALL — editor log idle 120 s, killing" >&2; break; fi
  elif [ "$seen" = 1 ]; then
    break
  fi
  [ "$(date +%s)" -ge "$DEADLINE" ] && { echo "cinematic_shot: TIMEOUT, killing" >&2; break; }
done

cleanup; trap - EXIT INT TERM
sleep 4   # screenshots write 2-4 s AFTER the process exits (runbook §3b)
echo
echo "cinematic_shot: newest shots in $SHOTDIR"
ls -t "$SHOTDIR"/*.png 2>/dev/null | head -8
echo "cinematic tier marker (should appear once):"
grep -m1 "LogSeaShieldGame.*SeaCinematic tier ACTIVE" "$ABSLOG" 2>/dev/null \
  || echo "  (no -SeaCinematic marker — check $ABSLOG)"
