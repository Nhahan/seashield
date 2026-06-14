#!/bin/zsh
# Launch the SeaShield survival game for hands-on play — you are the gun director.
#
#   client/SeaShield/Tools/play_game.sh
#
# The server runs the endless ASM wave engine (scenarios/game.scn); the UE client
# is your first-person fire-control console. The server is stopped when you quit.
#
# CONTROLS
#   Mouse            slew the launcher (azimuth / elevation)
#   F / Space / LMB  fire the unguided salvo down the current bore
#   [ / ]            salvo size  (rockets per volley)
#   ; / '            pattern dispersion (mrad)
#   Arrow keys       fine trim
#
# HOW TO PLAY
#   Put the amber IMPACT pipper on the cyan AIM ghost (where the threat will be
#   after the salvo's flight) and fire. The pipper turns green (SOLUTION) when
#   laid on. Lead the maneuvering missile — unguided rockets cannot correct, so
#   read the wind and the weave. A target that reaches the ship costs a life;
#   three leaks ends the run. The edge chevron points at a threat off-screen.
#
# Env overrides: UE=<engine path>  PORT=<tcp port>
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
UE="${UE:-/Users/Shared/Epic Games/UE_5.7}"
PROJ="$ROOT/client/SeaShield/SeaShield.uproject"
PORT=${PORT:-7777}

if [ ! -x "$ROOT/build/seashield_server" ]; then
  echo "server binary missing — build it first:  cmake --build build" >&2
  exit 1
fi

"$ROOT/build/seashield_server" --scenario "$ROOT/scenarios/game.scn" \
  --port "$PORT" --udp-port "$((PORT + 1))" &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT INT TERM
sleep 1

echo "SeaShield: gun director console up. Mouse to aim, F/Space/LMB to fire."
"$UE/Engine/Binaries/Mac/UnrealEditor" "$PROJ" -game -windowed -ResX=1920 -ResY=1080 \
  -SeaServer="127.0.0.1:$PORT" -SeaRole=solo
