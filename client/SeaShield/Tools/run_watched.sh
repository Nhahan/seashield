#!/bin/zsh
# run_watched.sh <stall_seconds> <logfile> -- <command...>
#
# Runs a long command (UE build / cook / capture) with a STALL WATCHDOG: its
# output goes to <logfile>, and if that file stops growing for <stall_seconds>
# while the command is still alive, the whole process tree is killed and the
# wrapper exits 124. So a hung BuildCookRun (observed: AutomationTool wedged at
# 0% CPU for 20+ min) fails fast with a clear marker instead of silently
# stalling. On normal completion it returns the command's own exit code.
#
# Pair with a Monitor on <logfile> for live progress; this wrapper is the
# enforcement (kill-on-stall), the Monitor is the visibility.
set -u
STALL=${1:?usage: run_watched.sh <stall_seconds> <logfile> -- <command...>}
LOG=${2:?logfile required}
shift 2
[ "${1:-}" = "--" ] && shift

: > "$LOG"
"$@" >> "$LOG" 2>&1 &
PID=$!

kill_tree() {  # recursively SIGKILL a pid and all descendants
  local p=$1 c
  for c in $(pgrep -P "$p" 2>/dev/null); do kill_tree "$c"; done
  kill -9 "$p" 2>/dev/null
}

last=0; quiet=0
while kill -0 "$PID" 2>/dev/null; do
  sleep 15
  cur=$(stat -f%z "$LOG" 2>/dev/null || echo 0)
  if [ "$cur" = "$last" ]; then quiet=$((quiet + 15)); else quiet=0; last=$cur; fi
  if [ "$quiet" -ge "$STALL" ]; then
    echo "run_watched: STALL — no output for ${quiet}s, killing process tree" >&2
    kill_tree "$PID"
    wait "$PID" 2>/dev/null
    exit 124
  fi
done
wait "$PID"; rc=$?
echo "run_watched: exited rc=$rc after $(stat -f%z "$LOG" 2>/dev/null || echo 0)B output"
exit $rc
