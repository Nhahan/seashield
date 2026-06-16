#!/bin/zsh
# perf_summary.sh <logfile>
#
# Reads a SeaShield -game run log and prints a 1440p60 frame-budget VERDICT.
# Every in-game test ends by piping its log through this (runbook §4/§5) so the
# optimize-or-not decision is built into the capture loop, not an after-the-fact
# manual log dig. perf_capture.sh calls it; you can also point it at any -game
# run's -abslog.
#
# WHY it keys on over-33.3% and p99, NOT avg or over-16.7%: under vsync the avg
# pins to 16.67 ms ("exactly 60 fps") regardless of GPU headroom (runbook §5).
# The per-frame DeltaTime jitters around the 16.667 ms vsync period, so even a
# perfect 60 fps run shows ~40% of frames a hair over 16.7 ms — over-16.7% is
# therefore informational, NOT a gate. A frame that genuinely misses its slot
# lands a full vsync later (~33 ms), so over-33.3% is the true missed-60fps rate
# and p99 is the tail. These come from the "PERF:" line EndPlay emits.
set -u
LOG=${1:?usage: perf_summary.sh <logfile>}
[ -r "$LOG" ] || { echo "perf_summary: cannot read $LOG" >&2; exit 2; }

PERF=$(grep 'PERF: frames=' "$LOG" | tail -1)
HITCHES=$(grep -c 'Hitch:' "$LOG")

if [ -z "$PERF" ]; then
  echo "perf_summary: no PERF line in $LOG" >&2
  echo "  (run too short? EndPlay needs >60 steady frames past the 8 s warmup —" >&2
  echo "   use -SeaQuit>=25. Or the run crashed before EndPlay.)" >&2
  grep -E 'Hitch:|Frame stats:' "$LOG" | tail -5
  exit 3
fi

# UE prepends "[ts][frame]LogSeaShieldWorld: " — strip up to our marker.
PERF=${PERF#*PERF: }
# [^a-z_] boundary so "avg=" does not also match inside "fps_avg=".
val() { echo "$PERF" | sed -E "s/.*[^a-z_]$1=([0-9.]+).*/\1/; s/^$1=([0-9.]+).*/\1/"; }
FRAMES=$(echo "$PERF" | sed -E 's/.*frames=([0-9]+).*/\1/')
AVG=$(val avg); P95=$(val p95); P99=$(val p99); MAXMS=$(val max)
FPS=$(val fps_avg); OVER16=$(val 'over16\.7'); OVER33=$(val 'over33\.3')

# Verdict (floats → awk). Gates: over-33.3% (true missed-60fps under vsync) + p99
# tail + hitches. over-16.7% is NOT gated (vsync jitter inflates it). A >100 ms
# hitch forces at least MARGINAL (a single stall a viewer will feel).
VERDICT=$(awk -v o33="$OVER33" -v p99="$P99" -v h="$HITCHES" 'BEGIN{
  # Gate: over-33.3% (true missed-60fps under vsync) + p99 within 1.5x the 16.67ms
  # budget (=25ms — the textbook "1% low" smoothness bound; a single frame dipping
  # to ~42fps is imperceptible) + no >100ms hitch. over-16.7% is NOT gated (vsync
  # jitter inflates it). The real fail signal is a frame slipping a full vsync to
  # ~33ms (over-33.3%).
  if (o33+0 < 1 && p99+0 <= 25 && h+0 == 0)        print "PASS";
  else if (o33+0 < 5 && p99+0 <= 34)               print "MARGIN";
  else                                              print "FLAG";
}')

echo "── SeaShield 1440p60 frame-budget summary ──────────────────────"
echo "  log:           $LOG"
echo "  frames:        $FRAMES  (steady-state, first 8 s warmup excluded)"
echo "  avg:           $AVG ms  (fps_avg $FPS)   ← vsync-pinned, NOT the gate"
echo "  p95 / p99:     $P95 / $P99 ms"
echo "  max:           $MAXMS ms"
echo "  over 16.7 ms:  $OVER16 %   (vsync jitter inflates this — informational)"
echo "  over 33.3 ms:  $OVER33 %   ← true missed-60fps rate (PRIMARY gate)"
echo "  hitches>100ms: $HITCHES"
case "$VERDICT" in
  PASS)   echo "  VERDICT:       ✅ PASS — solid 1440p60, has headroom for more quality";;
  MARGIN) echo "  VERDICT:       ⚠️  MARGINAL — at/near budget, headroom thin (watch the tail)";;
  *)      echo "  VERDICT:       ❌ FLAG — over budget, optimize (attribute with --gpu / -SeaProfileGPU)";;
esac

# If a ProfileGPU dump is present, surface the costliest passes (machine-readable
# attribution — no on-screen overlay screenshot needed).
if grep -qi 'ProfileGPU' "$LOG"; then
  echo "── GPU pass breakdown (ProfileGPU) ─────────────────────────────"
  grep -iE 'ms\b' "$LOG" | grep -iE 'LogRHI|ProfileGPU|Scene|Lumen|Water|Translucen|Cloud|PostProcess|ShadowDepth|BasePass|Reflection' \
    | tail -40
fi
