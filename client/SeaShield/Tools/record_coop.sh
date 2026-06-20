#!/bin/zsh
# Co-op multi-client demo: one server, three role-differentiated UE clients
# (Commander / Weapons / Observer) on screen at once — each recorded as its own
# window — PLUS a handful of headless dummy clients on the same server and a
# 4th montage panel tailing the live server log. Montaged into a single
# side-by-side clip. This is the visual proof of the hard requirement ("a C++
# server that takes several clients and processes them") and the P5 co-op DoD:
# the server fans one engagement out to 3 UE + 5 dummy stations at once, and
# the log panel shows the role attaches / udp binds on screen.
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

REC="$REPO/client/SeaShield/Saved/Demo"
mkdir -p "$REC"
SRVLOG="$REC/_coop_server.log"            # live server log -> 4th montage panel
"$REPO/build/seashield_server" --scenario "$REPO/scenarios/demo-fire.scn" --verbose \
  >"$SRVLOG" 2>&1 &
SERVER_PID=$!
# Clean up the server, the (long-lived -SeaQuit) UE clients, AND the headless
# dummy clients we attach below, on exit.
trap 'kill $SERVER_PID 2>/dev/null || true; pkill -f "StagedBuilds/Mac/SeaShield" 2>/dev/null || true; for p in ${DUMMY_PIDS:-}; do kill $p 2>/dev/null || true; done' EXIT

# Three seats, positioned left-to-right so recorder index 0/1/2 maps to
# Commander/Weapons/Observer. Only Weapons fires.
launch() { # role winx extra...
  "$APP" -windowed -ResX=$W -ResY=$H -WinX=$2 -WinY=$Y -nosplash \
    -dpcvars="$LOWQ" -SeaServer=127.0.0.1 -SeaRole=$1 -SeaQuit=$SECONDS_TOTAL "${@:3}" \
    >/dev/null 2>&1 &
}
launch commander 0
launch weapons   860 -SeaOrderDemo
launch observer  1720
echo "launched 3 UE clients; all run concurrently (the co-op proof is server-side)."

# Attach a handful of HEADLESS dummy clients to the SAME server during the
# capture: extra real stations the server must fan snapshots out to (and extra
# "role … attached / udp bound" lines in the 4th log panel). They're observers
# (the one armed seat belongs to the Weapons UE console), kept alive for the
# whole montage window. Pure server-side proof — they draw nothing on screen.
DUMMY="$REPO/build/dummyclient"
DUMMY_PIDS=""
if [ -x "$DUMMY" ]; then
  "$DUMMY" --port 7777 --clients 5 --role observer --duration $SECONDS_TOTAL \
    >/dev/null 2>&1 &
  DUMMY_PIDS="$!"
  echo "attached 5 headless dummy observers (server now drives 3 UE + 5 dummy stations)."
fi

# Let the windows come up and the engagement develop (salvo airborne).
sleep 28

# Capture each window SEQUENTIALLY — one SCStream at a time. ScreenCaptureKit
# stalls when three streams start at once (startCapture never returns); one at
# a time is rock-solid. The clients stay up the whole time (-SeaQuit large), so
# all three consoles are live during every clip; the clips are a few seconds
# apart on the same continuous engagement.
"$RECORDER" SeaShield $REC_SECONDS "$REC/_coop0.mov" 30 0
"$RECORDER" SeaShield $REC_SECONDS "$REC/_coop1.mov" 30 1
"$RECORDER" SeaShield $REC_SECONDS "$REC/_coop2.mov" 30 2

# 4-up montage with a labelled header. Labels go through a PIL-drawn title bar
# rather than ffmpeg drawtext — this Homebrew ffmpeg's drawtext refuses the
# filter ("text must be provided"), PIL is dependable. The 4th panel is the
# LIVE SERVER LOG (role attaches + udp binds + closes), the on-screen proof
# that one C++ server is fanning the engagement out to every station (3 UE +
# 5 dummy). It's a still PNG of the log tail, looped to the clip duration.
BAR="$REC/_titlebar.png"
LOGPANEL="$REC/_logpanel.png"
python3 - "$BAR" "$SRVLOG" "$LOGPANEL" <<'PY'
import os, sys
from PIL import Image, ImageDraw, ImageFont

bar_path, srvlog, logpanel = sys.argv[1], sys.argv[2], sys.argv[3]
PANELS = ["COMMANDER  ·  PPI", "WEAPONS  ·  PPI + FIRE CONTROL",
          "OBSERVER  ·  FREE VIEW", "SERVER LOG  ·  SESSIONS"]
col, n = 640, 4
W, H = col * n, 46
green = (120, 255, 140)

# --- title bar across all four panels ---
img = Image.new("RGB", (W, H), (8, 10, 9)); d = ImageDraw.Draw(img)
bold = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
path = bold if os.path.exists(bold) else "/System/Library/Fonts/Supplemental/Arial.ttf"
f = ImageFont.truetype(path, 22)
for i, t in enumerate(PANELS):
    bb = d.textbbox((0, 0), t, font=f); tw = bb[2] - bb[0]
    d.text((i * col + (col - tw) // 2, (H - (bb[3] - bb[1])) // 2 - 2), t,
           fill=green, font=f)
img.save(bar_path)

# --- server-log panel: tail the lines that prove multi-client fan-out ---
keep = ("attached", "udp bound", "closed", "sim server up")
lines = []
try:
    with open(srvlog, "r", errors="replace") as fh:
        for ln in fh:
            if any(k in ln for k in keep):
                lines.append(ln.rstrip("\n"))
except OSError:
    lines = ["(server log unavailable)"]
lines = lines[-22:] or ["(no session activity logged yet)"]

panel = Image.new("RGB", (col, 360), (6, 8, 7)); pd = ImageDraw.Draw(panel)
mono = "/System/Library/Fonts/Menlo.ttc"
mpath = mono if os.path.exists(mono) else path
mf = ImageFont.truetype(mpath, 12)
pd.text((10, 8), "live server log — one C++ server, many clients", fill=(90, 200, 110), font=mf)
y = 30
for ln in lines:
    pd.text((10, y), ln[:96], fill=green if "attached" in ln else (150, 210, 160), font=mf)
    y += 15
panel.save(logpanel)
PY
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$REC/_coop0.mov")
# 3 captured consoles + the looped server-log panel -> 2560x360 hstack.
ffmpeg -v error -i "$REC/_coop0.mov" -i "$REC/_coop1.mov" -i "$REC/_coop2.mov" \
  -loop 1 -t "$DUR" -i "$LOGPANEL" \
  -filter_complex "[0:v]scale=640:360[a];[1:v]scale=640:360[b];[2:v]scale=640:360[c];[3:v]scale=640:360[d];[a][b][c][d]hstack=inputs=4[v]" \
  -map "[v]" -c:v libx264 -preset slow -crf 19 -pix_fmt yuv420p -y "$REC/_grid.mp4"
ffmpeg -v error -loop 1 -i "$BAR" -i "$REC/_grid.mp4" \
  -filter_complex "[0:v]trim=duration=$DUR,setpts=PTS-STARTPTS[tb];[tb][1:v]vstack=inputs=2[v]" \
  -map "[v]" -c:v libx264 -preset slow -crf 19 -pix_fmt yuv420p -movflags +faststart -y "$OUT"
rm -f "$REC/_coop0.mov" "$REC/_coop1.mov" "$REC/_coop2.mov" "$REC/_grid.mp4" "$BAR" "$LOGPANEL"
echo "co-op demo written: $OUT"
