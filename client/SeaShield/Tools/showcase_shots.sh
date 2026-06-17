#!/bin/zsh
# showcase_shots.sh [SUFFIX] — reproduce the 4 critique reference framings at the
# CINEMATIC tier so before/after is apples-to-apples. SUFFIX (default "_after") is
# appended to the show_N names. The 3 beauty shots are idle (static sea/sky); the
# 4th is captured on a live airburst (-SeaShotOnBurst).
#   show_1_hero  289000,293200,1600,-4,32   3/4 hero at ~110 m
#   show_2_hull  295000,297000,820,-4,30     close hull, low grazing
#   show_3_godrays 313000,309000,2800,7,216  backlit toward the low sun
#   show_4_combat  -SeaShotOnBurst           intercept burst (gameplay)
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
UE="${UE:-/Users/Shared/Epic Games/UE_5.7}/Engine/Binaries/Mac/UnrealEditor"
PROJ="$ROOT/client/SeaShield/SeaShield.uproject"
SHOTDIR="$ROOT/client/SeaShield/Saved/Screenshots/MacEditor"
CINE="$(grep -v '^[[:space:]]*#' "$(dirname "$0")/cinematic.cvars" | grep -v '^[[:space:]]*$' | tr -d '[:blank:]' | paste -sd, -)"
SUF="${1:-_after}"

idle_shot() {  # $1=port $2=cam $3=outname
  "$ROOT/client/SeaShield/Tools/cinematic_shot.sh" --idle --shot 9 --cam "$2" --port "$1" >/dev/null 2>&1
  cp "$SHOTDIR/SeaShot.png" "$SHOTDIR/$3" 2>/dev/null && echo "  $SHOTDIR/$3"
}

echo "=== showcase captures (suffix=$SUF) ==="
idle_shot 7855 "289000,293200,1600,-4,32"  "show_1_hero${SUF}.png"
idle_shot 7857 "295000,297000,820,-4,30"   "show_2_hull${SUF}.png"
idle_shot 7859 "313000,309000,2800,7,216"  "show_3_godrays${SUF}.png"

# Combat burst: -SeaShotOnBurst shoots when the first airburst goes off.
PORT=7861; LOG=/tmp/ss_show_burst.log; : > "$LOG"; MARKER="SeaServer=127.0.0.1:$PORT"
rm -f "$SHOTDIR"/SeaBurst*.png
python3 -c "import os,sys; os.setsid(); os.execv(sys.argv[1], sys.argv[1:])" \
  "$ROOT/build/seashield_server" --scenario "$ROOT/scenarios/game.scn" --port "$PORT" --udp-port "$((PORT+1))" >/tmp/ss_show_srv.log 2>&1 &
SRV=$!; sleep 1.5
"$UE" "$PROJ" -game -windowed -ResX=2560 -ResY=1440 -nosplash "-SeaServer=127.0.0.1:$PORT" \
  -SeaRole=solo -SeaGamePlay -SeaShotOnBurst -SeaCinematic "-dpcvars=$CINE" "-abslog=$LOG" >/dev/null 2>&1 &
seen=0; last=0; quiet=0; dl=$(( $(date +%s) + 150 ))
while :; do sleep 3
  if pgrep -f "$MARKER" >/dev/null 2>&1; then seen=1; cur=$(stat -f%z "$LOG" 2>/dev/null||echo 0); [ "$cur" = "$last" ] && quiet=$((quiet+3)) || { quiet=0; last=$cur; }; [ "$quiet" -ge 80 ] && { pkill -f "UnrealEditor.*$MARKER"; break; }
  elif [ "$seen" = 1 ]; then break; fi
  [ "$(date +%s)" -ge "$dl" ] && { pkill -f "UnrealEditor.*$MARKER"; break; }; done
pkill -f "UnrealEditor.*$MARKER" 2>/dev/null; pkill -f "seashield_server.*--port $PORT( |\$)" 2>/dev/null; kill "$SRV" 2>/dev/null; sleep 4
ls "$SHOTDIR"/SeaBurst*.png 2>/dev/null | head -1 | while read f; do cp "$f" "$SHOTDIR/show_4_combat${SUF}.png"; echo "  $SHOTDIR/show_4_combat${SUF}.png"; done
echo "=== done ==="
