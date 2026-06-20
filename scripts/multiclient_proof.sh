#!/usr/bin/env bash
# Headless multi-client proof — the project's hard requirement made auditable.
#
#   scripts/multiclient_proof.sh
#
# Boots ONE authoritative sim server and drives many real C++ clients at it
# concurrently, then parses the server log + each client's self-report into a
# PASS/FAIL verdict. Everything here is headless (no GUI, no UE) and self-
# verifying — it is the CLI twin of the ctest discipline, standing in for the
# defense-industry "a C++ server that takes several clients and processes them"
# requirement. It proves four things with real evidence:
#
#   1. CONCURRENCY   — ~11 sockets attached at once (3 roles + an 8-client load
#                      batch); peak is reconstructed from the server log.
#   2. ROLE ATTACH   — commander / weapons / observer each attach (one armed
#                      seat, N spectators), logged server-side.
#   3. ROLE EXCLUSIVITY — a SECOND weapons client is rejected (kRoleTaken):
#                      the server closes its transport "handshake rejected" and
#                      the client exits nonzero.
#   4. SLOW-CLIENT ISOLATION — a UDP path degraded by the chaos proxy keeps the
#                      slow client's downlink lossy while every healthy client
#                      keeps advancing; the authoritative server-side send-cap
#                      eviction is proven by the SlowClientIsEvicted ctest.
#
# Artifacts (server log + parsed summary) land under docs/reports/data/.
# Exit 0 only when every check passes. Cleans up by pid/port-match on any exit.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build"
SERVER="$BUILD/seashield_server"
DUMMY="$BUILD/dummyclient"
PROXY="$BUILD/netproxy"
SCENARIO="$REPO/scenarios/demo-fire.scn"

# Unique high ports so we never clash with a stray server/proxy.
TCP_PORT=7801
UDP_PORT=7802
PROXY_PORT=7903

DATA_DIR="$REPO/docs/reports/data"
LOG="$DATA_DIR/multiclient-proof.log"          # full server log (the artifact)
SUMMARY="$DATA_DIR/multiclient-proof.summary.txt"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ss_mcproof.XXXXXX")"

DURATION=18                 # each dummy stays attached this long
OVERALL_TIMEOUT=180         # hard ceiling for the whole run (s)
START_EPOCH=$(date +%s)

SERVER_PID=""
PROXY_PID=""
declare -a BG_PIDS=()       # background dummies we may need to reap

# ---- robust cleanup (runs on normal exit, error, or signal) ----------------
cleanup() {
  local code=$?
  [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
  [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
  for p in "${BG_PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  # Port-matched belt-and-suspenders: only the processes WE bound. Never a
  # broad pkill (would cross-kill a parallel UE/server session).
  pkill -f "seashield_server.*--port $TCP_PORT" 2>/dev/null || true
  pkill -f "netproxy.*--listen $PROXY_PORT" 2>/dev/null || true
  rm -rf "$WORK" 2>/dev/null || true
  return $code
}
trap cleanup EXIT INT TERM

die() { echo "FATAL: $*" >&2; exit 1; }

watchdog() {  # abort the whole run if we blow the overall budget
  if [ $(( $(date +%s) - START_EPOCH )) -gt $OVERALL_TIMEOUT ]; then
    die "overall timeout (${OVERALL_TIMEOUT}s) exceeded"
  fi
}

# ---- preflight -------------------------------------------------------------
mkdir -p "$DATA_DIR"
for bin in "$SERVER" "$DUMMY" "$PROXY"; do
  [ -x "$bin" ] || die "missing binary: $bin (build with: cmake --build build -j8)"
done
[ -f "$SCENARIO" ] || die "missing scenario: $SCENARIO"

echo "== SeaShield multi-client proof =="
echo "repo=$REPO  tcp=$TCP_PORT udp=$UDP_PORT proxy=$PROXY_PORT  dur=${DURATION}s"

# ---- (a) boot ONE fresh server, wait until it is actually listening --------
"$SERVER" --scenario "$SCENARIO" --port "$TCP_PORT" --udp-port "$UDP_PORT" --verbose \
  >"$LOG" 2>&1 &
SERVER_PID=$!
echo "server pid=$SERVER_PID -> $LOG"

up=0
for _ in $(seq 1 100); do          # up to ~10 s
  watchdog
  if grep -q "sim server up" "$LOG" 2>/dev/null; then up=1; break; fi
  kill -0 "$SERVER_PID" 2>/dev/null || die "server exited during startup; log:\n$(cat "$LOG")"
  sleep 0.1
done
[ "$up" = 1 ] || die "server never reported 'sim server up'; log:\n$(cat "$LOG")"
echo "server is listening."

# ---- (b) launch the concurrent fleet ---------------------------------------
# One armed weapons seat (fires a 16-round salvo), a commander, an observer,
# and an 8-client observer load batch -> ~11 sockets on the server at once.
echo "launching concurrent fleet (commander + weapons + observer + 8x load)..."
"$DUMMY" --port "$TCP_PORT" --role commander --duration "$DURATION" \
  >"$WORK/commander.out" 2>&1 & BG_PIDS+=($!); CMD_PID=$!
"$DUMMY" --port "$TCP_PORT" --role weapons --fire-after 2 --salvo 16 --duration "$DURATION" \
  >"$WORK/weapons.out" 2>&1 & BG_PIDS+=($!); WPN_PID=$!
"$DUMMY" --port "$TCP_PORT" --role observer --duration "$DURATION" \
  >"$WORK/observer.out" 2>&1 & BG_PIDS+=($!); OBS_PID=$!
"$DUMMY" --port "$TCP_PORT" --clients 8 --role observer --duration "$DURATION" \
  >"$WORK/load.out" 2>&1 & BG_PIDS+=($!); LOAD_PID=$!

# Give the fleet a moment to all attach, then snapshot the peak from the log.
sleep 4
watchdog

# ---- (c) role exclusivity: a SECOND weapons client must be rejected --------
# This one runs while the first weapons seat is live -> kRoleTaken. It connects
# direct (no token) so the server closes its transport "handshake rejected".
echo "probing role exclusivity (second weapons seat -> expect kRoleTaken)..."
"$DUMMY" --port "$TCP_PORT" --role weapons --duration 3 >"$WORK/weapons2.out" 2>&1
DUP_WEAPONS_EXIT=$?
echo "second-weapons exit=$DUP_WEAPONS_EXIT (nonzero expected: rejected)"

# ---- (d) slow-client demo via the chaos proxy ------------------------------
# Stand up a heavily-degrading UDP proxy and route a dummy's UDP through it.
# Its downlink should be visibly lossy (fewer snapshot ticks / lower kbps) while
# every direct client keeps advancing — graceful degradation + isolation. The
# proxy only mangles the lossy client's path; it cannot harm the others.
echo "launching chaos proxy (loss/dup/delay/jitter) for the slow-client demo..."
"$PROXY" --listen "$PROXY_PORT" --upstream-port "$UDP_PORT" --upstream-host 127.0.0.1 \
  --loss 0.45 --dup 0.1 --delay-ms 120 --jitter-ms 120 --seed 7 \
  >"$WORK/proxy.out" 2>&1 &
PROXY_PID=$!
sleep 0.5
kill -0 "$PROXY_PID" 2>/dev/null || die "chaos proxy failed to start; out:\n$(cat "$WORK/proxy.out")"

# A baseline observer on the DIRECT path, plus the degraded one through the
# proxy, both for the same window — so we can compare like-for-like.
"$DUMMY" --port "$TCP_PORT" --role observer --duration 10 \
  >"$WORK/direct.out" 2>&1 & BG_PIDS+=($!); DIRECT_PID=$!
"$DUMMY" --port "$TCP_PORT" --role observer --udp-port "$PROXY_PORT" --duration 10 \
  >"$WORK/degraded.out" 2>&1 & BG_PIDS+=($!); DEGRADED_PID=$!

# ---- wait for the whole fleet to finish (bounded) --------------------------
echo "waiting for clients to complete..."
for pid in "$CMD_PID" "$WPN_PID" "$OBS_PID" "$LOAD_PID"; do
  wait "$pid"; eval "EXIT_$pid=$?"
done
CMD_EXIT=$(eval echo \$EXIT_$CMD_PID)
WPN_EXIT=$(eval echo \$EXIT_$WPN_PID)
OBS_EXIT=$(eval echo \$EXIT_$OBS_PID)
LOAD_EXIT=$(eval echo \$EXIT_$LOAD_PID)
wait "$DIRECT_PID"; DIRECT_EXIT=$?
wait "$DEGRADED_PID"; DEGRADED_EXIT=$?
watchdog

# Stop the server so its log is final & flushed, then parse it.
kill "$SERVER_PID" 2>/dev/null || true
for _ in $(seq 1 50); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.1; done
SERVER_PID=""   # already reaped; don't double-kill in cleanup

# ---- (e) authoritative send-cap eviction proof (ctest) ---------------------
# The CLI dummy reads promptly, so it does NOT itself trip the live send-cap
# close. The rigorous server-side eviction proof is the dedicated unit test:
# a client that connects, sends, and never reads -> evicted, with a healthy
# peer left completely unaffected (isolation). Run it fresh here.
echo "running authoritative slow-client eviction ctest..."
EVICT_LOG="$WORK/evict_ctest.out"
if ctest --test-dir "$BUILD" -R "SlowClientIsEvictedWithoutHarmingOthers" \
     --output-on-failure >"$EVICT_LOG" 2>&1; then
  EVICT_PASS=1
else
  EVICT_PASS=0
fi
EVICT_LINE="$(grep -E "tests passed|Passed|Failed" "$EVICT_LOG" | tail -1)"

# ---------------------------------------------------------------------------
# PARSE the server log into evidence.
# ---------------------------------------------------------------------------
# Peak concurrency: walk the log in order; every "role ... attached" is +1 on
# that transport, every "transport N closed" is -1. Track the running max.
read -r PEAK ATTACHES CLOSES UDP_BINDS <<EOF
$(awk '
  /attached to transport/ { cur++; if (cur>peak) peak=cur; attaches++ }
  /transport [0-9]+ closed/ { cur--; closes++ }
  /udp bound/ { binds++ }
  END { printf "%d %d %d %d", peak, attaches, closes, binds }
' "$LOG")
EOF

# The three named role attaches (role 1=commander, 2=weapons, 0=observer).
ROLE_CMD="$(grep -m1 -E "role 1 attached to transport" "$LOG" || true)"
ROLE_WPN="$(grep -m1 -E "role 2 attached to transport" "$LOG" || true)"
ROLE_OBS="$(grep -m1 -E "role 0 attached to transport" "$LOG" || true)"

# kRoleTaken evidence: the rejected handshake close line.
REJECT_LINE="$(grep -m1 -E "transport [0-9]+ closed: handshake rejected" "$LOG" || true)"
REJECT_COUNT="$(grep -cE "closed: handshake rejected" "$LOG" || true)"

# Slow-client numbers: pull "snapshots" + "kbps" from each dummy's report row.
# Report row columns: client role snapshots ticks events asm/delta kB kbps dup status
dummy_metric() { # file column(1=snapshots,2=ticks ... by header) -> value of client 0
  awk 'NR>1 && $1=="0" {print; exit}' "$1"
}
DIRECT_ROW="$(grep -E "^0 " "$WORK/direct.out" | head -1)"
DEGRADED_ROW="$(grep -E "^0 " "$WORK/degraded.out" | head -1)"
DIRECT_SNAPS="$(echo "$DIRECT_ROW" | awk '{print $3}')"
DEGRADED_SNAPS="$(echo "$DEGRADED_ROW" | awk '{print $3}')"
DIRECT_KBPS="$(echo "$DIRECT_ROW" | awk '{print $8}')"
DEGRADED_KBPS="$(echo "$DEGRADED_ROW" | awk '{print $8}')"
DIRECT_STATUS="$(echo "$DIRECT_ROW" | awk '{print $NF}')"

# ---------------------------------------------------------------------------
# VERDICTS
# ---------------------------------------------------------------------------
PASS=1
chk() { # label condition
  if [ "$2" = 1 ]; then echo "  PASS  $1"; else echo "  FAIL  $1"; PASS=0; fi
}

# 1. concurrency: peak must reach the full fleet (3 named + 8 load = 11),
#    allowing for the brief rejected second-weapons not counting.
C_CONC=$([ "${PEAK:-0}" -ge 11 ] && echo 1 || echo 0)
# 2. all three role attaches present.
C_ROLES=$([ -n "$ROLE_CMD" ] && [ -n "$ROLE_WPN" ] && [ -n "$ROLE_OBS" ] && echo 1 || echo 0)
# 3. role exclusivity: a rejected handshake AND the second-weapons exited nonzero.
C_REJECT=$([ -n "$REJECT_LINE" ] && [ "$DUP_WEAPONS_EXIT" -ne 0 ] && echo 1 || echo 0)
# 4a. the healthy fleet all exited 0 (welcomed + advancing snapshots).
C_FLEET=$([ "$CMD_EXIT" = 0 ] && [ "$WPN_EXIT" = 0 ] && [ "$OBS_EXIT" = 0 ] && [ "$LOAD_EXIT" = 0 ] && echo 1 || echo 0)
# 4b. graceful degradation: the direct path stayed healthy while the degraded
#     path took visibly fewer snapshots (lossy downlink, isolated to it).
C_DEGRADE=$([ "$DIRECT_STATUS" = ok ] && \
            [ -n "$DIRECT_SNAPS" ] && [ -n "$DEGRADED_SNAPS" ] && \
            [ "${DEGRADED_SNAPS:-0}" -lt "${DIRECT_SNAPS:-0}" ] && echo 1 || echo 0)
# 4c. authoritative eviction unit test passed.
C_EVICT=$([ "$EVICT_PASS" = 1 ] && echo 1 || echo 0)

# ---------------------------------------------------------------------------
# SUMMARY (stdout + artifact)
# ---------------------------------------------------------------------------
{
echo "================ SeaShield multi-client proof — SUMMARY ================"
echo "scenario   : $SCENARIO"
echo "ports      : tcp=$TCP_PORT udp=$UDP_PORT proxy=$PROXY_PORT"
echo
echo "[1] CONCURRENCY"
echo "    peak concurrent attached transports : ${PEAK:-0}   (target >= 11)"
echo "    total role attaches / closes / udp-binds : $ATTACHES / $CLOSES / $UDP_BINDS"
chk "concurrency peak >= 11 sockets" "$C_CONC"
echo
echo "[2] ROLE ATTACH (one armed seat + spectators)"
echo "    commander : ${ROLE_CMD:-<none>}"
echo "    weapons   : ${ROLE_WPN:-<none>}"
echo "    observer  : ${ROLE_OBS:-<none>}"
chk "all three roles attached" "$C_ROLES"
echo
echo "[3] ROLE EXCLUSIVITY (second weapons -> kRoleTaken)"
echo "    server    : ${REJECT_LINE:-<none>}"
echo "    rejected-handshake count : $REJECT_COUNT"
echo "    second-weapons dummy exit: $DUP_WEAPONS_EXIT (nonzero = rejected)"
chk "second weapons rejected (kRoleTaken)" "$C_REJECT"
echo
echo "[4] SLOW-CLIENT ISOLATION"
echo "    healthy fleet exits  : commander=$CMD_EXIT weapons=$WPN_EXIT observer=$OBS_EXIT load(8x)=$LOAD_EXIT"
chk "entire healthy fleet welcomed + advancing (exit 0)" "$C_FLEET"
echo "    direct-path observer   : snapshots=${DIRECT_SNAPS:-?} kbps=${DIRECT_KBPS:-?} status=${DIRECT_STATUS:-?}"
echo "    degraded-path observer : snapshots=${DEGRADED_SNAPS:-?} kbps=${DEGRADED_KBPS:-?}  (via chaos proxy: loss .45 dup .1 delay 120ms jitter 120ms)"
chk "chaos-degraded downlink took fewer snapshots while direct stayed ok" "$C_DEGRADE"
echo "    eviction unit test     : ${EVICT_LINE:-<no result>}"
chk "authoritative send-cap eviction (SlowClientIsEvicted ctest)" "$C_EVICT"
echo
echo "NOTE on the slow-client proof: the CLI dummy reads its socket promptly, so"
echo "it does NOT itself trip the server's live send-cap close. The chaos-proxy"
echo "run above proves GRACEFUL DEGRADATION + ISOLATION (the degraded client's"
echo "downlink suffers loss while every healthy client keeps advancing). The"
echo "rigorous server-side EVICTION (a never-reading client overflows its"
echo "per-session SendQueue and is closed, leaving a healthy peer untouched) is"
echo "proven by the SlowClientIsEvictedWithoutHarmingOthers integration test,"
echo "run fresh above."
echo
if [ "$PASS" = 1 ]; then
  echo "RESULT: PASS — all four claims backed by real evidence."
else
  echo "RESULT: FAIL — see failing checks above."
fi
echo "full server log: $LOG"
echo "======================================================================="
} | tee "$SUMMARY"

[ "$PASS" = 1 ] || exit 1
exit 0
