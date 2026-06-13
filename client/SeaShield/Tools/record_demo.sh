#!/bin/zsh
# One-take demo recording: server + packaged client + WINDOW-ONLY capture
# (record_window.swift / ScreenCaptureKit) + trim/encode.
#
#   ./record_demo.sh [output.mp4]
#
# Window capture composites just the game window's layer, so OS dialogs and
# notifications floating above it never reach the recording — display capture
# (screencapture -v) grabs those overlays (verified the hard way). The
# recording is realtime against the live server; offline frame dumps
# (-DumpMovie) run off wall-clock and desync the event-driven VFX from the
# entity stream, so they are not an option for a server-synced client.
#
# NOTE: the recorder binary needs the Screen Recording permission of the
# invoking terminal, and a shell with WindowServer access — sandboxed or
# ssh/headless shells cannot open the connection (CGS_REQUIRE_INIT abort).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
TOOLS="$REPO/client/SeaShield/Tools"
APP="$REPO/client/SeaShield/Saved/StagedBuilds/Mac/SeaShield.app/Contents/MacOS/SeaShield"
OUT="${1:-$REPO/client/SeaShield/Saved/Demo/seashield_demo.mp4}"
RAW="$(mktemp -t seashield_demo).mov"
RECORDER="${TMPDIR:-/tmp}/seashield_record_window"
GAME_SECONDS=78    # boot ~4 s + salvo t~24 + ToF ~16 + splashes/tally
TRIM_START=2       # cut the boot frames (PTS starts at first rendered frame)

mkdir -p "$(dirname "$OUT")"
[ -x "$APP" ] || { echo "staged build missing — run BuildCookRun first"; exit 1; }
swiftc -O "$TOOLS/record_window.swift" -o "$RECORDER"

"$REPO/build/seashield_server" --scenario "$REPO/scenarios/demo-fire.scn" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

"$APP" -windowed -ResX=2560 -ResY=1440 -nosplash -SeaServer=127.0.0.1 \
    -SeaRole=solo -SeaFire=16 -SeaShot=999 -SeaQuit=$GAME_SECONDS &
GAME_PID=$!
# The recorder polls for the window, records until the game window closes.
"$RECORDER" SeaShield $((GAME_SECONDS + 6)) "$RAW"
wait $GAME_PID 2>/dev/null || true

# Crop the 32 px title bar (capture is the whole window incl. chrome).
ffmpeg -v error -ss "$TRIM_START" -i "$RAW" -an -vf "crop=2560:1440:0:32" \
    -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -movflags +faststart \
    -y "$OUT"
rm -f "$RAW"
echo "demo written: $OUT"
