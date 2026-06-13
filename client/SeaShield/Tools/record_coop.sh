#!/bin/zsh
# Co-op multi-client demo: one server, three role-differentiated clients
# (Commander / Weapons / Observer) on screen at once, each recorded as its own
# window, montaged into a single side-by-side clip. This is the visual proof of
# the hard requirement ("a server that takes several clients and processes
# them") and the P5 co-op DoD.
#
#   ./record_coop.sh [out.mp4]
#
# Same window-capture recorder as record_demo.sh (OS overlays never enter the
# frame). Clients run at low scalability + small windows so three full UE
# instances stay smooth on one machine. Needs a shell with Screen Recording
# permission and WindowServer access (not a sandboxed/headless shell).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
TOOLS="$REPO/client/SeaShield/Tools"
APP="$REPO/client/SeaShield/Saved/StagedBuilds/Mac/SeaShield.app/Contents/MacOS/SeaShield"
OUT="${1:-$REPO/client/SeaShield/Saved/Demo/seashield_coop.mp4}"
RECORDER="${TMPDIR:-/tmp}/seashield_record_window"
SECONDS_TOTAL=150            # clients stay up through all three sequential clips
REC_SECONDS=12               # per-console clip length
W=840; H=472; Y=120          # per-client window geometry
LOWQ="sg.ViewDistanceQuality=2,sg.ShadowQuality=2,sg.GlobalIlluminationQuality=1,sg.ReflectionQuality=1,sg.PostProcessQuality=2,sg.EffectsQuality=2,sg.AntiAliasingQuality=2"

mkdir -p "$(dirname "$OUT")"
[ -x "$APP" ] || { echo "staged build missing — run BuildCookRun first"; exit 1; }
swiftc -O "$TOOLS/record_window.swift" -o "$RECORDER"

"$REPO/build/seashield_server" --scenario "$REPO/scenarios/demo-fire.scn" &
SERVER_PID=$!
# Clean up the server AND the (long-lived -SeaQuit) clients on exit.
trap 'kill $SERVER_PID 2>/dev/null || true; pkill -f "StagedBuilds/Mac/SeaShield" 2>/dev/null || true' EXIT

# Three seats, positioned left-to-right so recorder index 0/1/2 maps to
# Commander/Weapons/Observer. Only Weapons fires.
launch() { # role winx extra...
  "$APP" -windowed -ResX=$W -ResY=$H -WinX=$2 -WinY=$Y -nosplash \
    -dpcvars="$LOWQ" -SeaServer=127.0.0.1 -SeaRole=$1 -SeaQuit=$SECONDS_TOTAL "${@:3}" \
    >/dev/null 2>&1 &
}
launch commander 0
launch weapons   860 -SeaFire=16
launch observer  1720
echo "launched 3 clients; all run concurrently (the co-op proof is server-side)."
# Let the windows come up and the engagement develop (salvo airborne).
sleep 28

# Capture each window SEQUENTIALLY — one SCStream at a time. ScreenCaptureKit
# stalls when three streams start at once (startCapture never returns); one at
# a time is rock-solid. The clients stay up the whole time (-SeaQuit large), so
# all three consoles are live during every clip; the clips are a few seconds
# apart on the same continuous engagement.
REC="$REPO/client/SeaShield/Saved/Demo"
"$RECORDER" SeaShield $REC_SECONDS "$REC/_coop0.mov" 30 0
"$RECORDER" SeaShield $REC_SECONDS "$REC/_coop1.mov" 30 1
"$RECORDER" SeaShield $REC_SECONDS "$REC/_coop2.mov" 30 2

# Side-by-side montage with a labelled header. Labels go through a PIL-drawn
# title bar rather than ffmpeg drawtext — this Homebrew ffmpeg's drawtext
# refuses the filter ("text must be provided"), PIL is dependable.
BAR="$REC/_titlebar.png"
python3 - "$BAR" <<'PY'
import os, sys
from PIL import Image, ImageDraw, ImageFont
W, H, col = 1920, 46, 640
img = Image.new("RGB", (W, H), (8, 10, 9)); d = ImageDraw.Draw(img)
bold = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
path = bold if os.path.exists(bold) else "/System/Library/Fonts/Supplemental/Arial.ttf"
f = ImageFont.truetype(path, 22)
for i, t in enumerate(["COMMANDER  ·  PPI", "WEAPONS  ·  PPI + FIRE CONTROL",
                       "OBSERVER  ·  FREE VIEW"]):
    bb = d.textbbox((0, 0), t, font=f); tw = bb[2] - bb[0]
    d.text((i * col + (col - tw) // 2, (H - (bb[3] - bb[1])) // 2 - 2), t,
           fill=(120, 255, 140), font=f)
img.save(sys.argv[1])
PY
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$REC/_coop0.mov")
ffmpeg -v error -i "$REC/_coop0.mov" -i "$REC/_coop1.mov" -i "$REC/_coop2.mov" \
  -filter_complex "[0:v]scale=640:360[a];[1:v]scale=640:360[b];[2:v]scale=640:360[c];[a][b][c]hstack=inputs=3[v]" \
  -map "[v]" -c:v libx264 -preset slow -crf 19 -pix_fmt yuv420p -y "$REC/_grid.mp4"
ffmpeg -v error -loop 1 -i "$BAR" -i "$REC/_grid.mp4" \
  -filter_complex "[0:v]trim=duration=$DUR,setpts=PTS-STARTPTS[tb];[tb][1:v]vstack=inputs=2[v]" \
  -map "[v]" -c:v libx264 -preset slow -crf 19 -pix_fmt yuv420p -movflags +faststart -y "$OUT"
rm -f "$REC/_coop0.mov" "$REC/_coop1.mov" "$REC/_coop2.mov" "$REC/_grid.mp4" "$BAR"
echo "co-op demo written: $OUT"
