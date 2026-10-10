#!/usr/bin/env bash
# DaliNira safe GUI / engine test runner.
#
# GUI tests create real windows and start real engine processes (QtWebEngine,
# WebKitGTK web/network processes) that use the GPU. On a workstation — in
# particular a hybrid Intel + NVIDIA laptop — they must run under these rules:
#
#   * only the real desktop display; never Xvfb or another virtual display,
#     and no GPU vendor / driver override variables;
#   * one GUI test at a time, guarded by a lock across all sessions;
#   * a hard timeout per test that terminates the whole process tree
#     (the test runs in its own session; every WebKit / QtWebEngine child
#     process in it is terminated as well);
#   * no retries and no stress loops: every test runs exactly once;
#   * before the next test, all child processes and any core dump processing
#     have finished;
#   * stop immediately when a test fails, times out, or when the kernel log
#     shows a new segfault or GPU driver error, a process crashed, or the
#     compositor reported a stall — even if the test itself reported success;
#   * no core dumps: processing one WebKit core dump takes systemd-coredump
#     ~5.5 GB of memory and stalled the desktop. Crashes are still logged and
#     detected (--keep-cores keeps them for debugging);
#   * a 1 Hz resource monitor (scripts/gui-test-monitor.py) records CPU,
#     PSS/private memory, WebKit process and window counts, GPU engine time
#     and memory pressure to build-dalinira/gui-test-logs/, and stops the test when
#     it exceeds a limit (e.g. more than one view's web processes, low
#     available memory, memory pressure);
#   * local deterministic pages by default. Opt-in tiers, only on request:
#       --manual           window-manager cases (media fullscreen)
#       --live             live sites (DuckDuckGo, YouTube)
#       --memory-profile   the memory profiler (live sites)
#
# Usage:
#   scripts/safe-gui-test.sh [--build-dir DIR] [--manual] [--live] [--keep-cores] [TEST...]
#   scripts/safe-gui-test.sh [--build-dir DIR] --memory-profile chromium|webkit
#   scripts/safe-gui-test.sh --list
#
# Without TEST names, every ctest test labelled "gui" runs, in order.
# A plain `ctest --test-dir build-dalinira` skips the GUI tier (it stays invisible).

set -u

BUILD_DIR="${DALINIRA_BUILD_DIR:-build-dalinira}"
LIVE=0
MANUAL=0
KEEP_CORES=0
PROFILE_ENGINE=""
LIST_ONLY=0
TESTS=()
DEFAULT_TIMEOUT=300       # seconds, per test, when ctest has no TIMEOUT
PROFILE_TIMEOUT=900
CLEANUP_WAIT=10           # seconds for engine processes to follow a finished test out
COREDUMP_WAIT=180         # seconds for systemd-coredump to finish

die() { echo "safe-gui-test: STOP: $*" >&2; exit 2; }
note() { echo "safe-gui-test: $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --live) LIVE=1; shift ;;
    --manual) MANUAL=1; shift ;;
    --keep-cores) KEEP_CORES=1; shift ;;
    --memory-profile) PROFILE_ENGINE="${2:-}"; shift 2 ;;
    --list) LIST_ONLY=1; shift ;;
    -h|--help) sed -n '2,44p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) TESTS+=("$1"); shift ;;
  esac
done

[ -d "$BUILD_DIR" ] || die "build directory '$BUILD_DIR' not found"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="$BUILD_DIR/gui-test-logs"
mkdir -p "$LOG_DIR"

# The GUI tier runs only when enabled (scripts/gui-test-gate.sh), and never
# with core dumps unless asked for.
export DALINIRA_GUI_TESTS=1
if [ "$KEEP_CORES" = 1 ]; then
  export DALINIRA_GUI_KEEP_CORES=1
else
  ulimit -c 0
fi
# The default tier allows one view (a navigation may briefly overlap two web
# processes). The manual tier raises the budget; the profiler, tabs.
if [ "$MANUAL" = 1 ]; then
  export DALINIRA_GUI_MANUAL=1
  export DALINIRA_GUI_MAX_WEB_PROCESSES="${DALINIRA_GUI_MAX_WEB_PROCESSES:-4}"
fi
[ -n "$PROFILE_ENGINE" ] && export DALINIRA_GUI_MAX_WEB_PROCESSES="${DALINIRA_GUI_MAX_WEB_PROCESSES:-16}"

gui_tests() {
  ctest --test-dir "$BUILD_DIR" -L '^gui$' --show-only=json-v1 2>/dev/null |
    python3 -c 'import json,sys; [print(t["name"]) for t in json.load(sys.stdin)["tests"]]'
}

# A test may declare its own WebKit web process budget in its ctest
# ENVIRONMENT (DALINIRA_GUI_MAX_WEB_PROCESSES), e.g. a two-tab test.
test_web_process_budget() {
  ctest --test-dir "$BUILD_DIR" -R "^$1\$" --show-only=json-v1 2>/dev/null | python3 -c '
import json, sys
tests = json.load(sys.stdin)["tests"]
env = [p["value"] for p in tests[0].get("properties", []) if p["name"] == "ENVIRONMENT"] if tests else []
for item in (env[0] if env else []):
    if item.startswith("DALINIRA_GUI_MAX_WEB_PROCESSES="):
        print(item.split("=", 1)[1])'
}

test_timeout() {
  ctest --test-dir "$BUILD_DIR" -R "^$1\$" --show-only=json-v1 2>/dev/null | python3 -c '
import json, sys
tests = json.load(sys.stdin)["tests"]
t = [p["value"] for p in tests[0].get("properties", []) if p["name"] == "TIMEOUT"] if tests else []
print(int(float(t[0])) if t else 0)'
}

if [ "$LIST_ONLY" = 1 ]; then gui_tests; exit 0; fi

# ---------------------------------------------------------------------------
# Environment checks: real desktop only, no GPU overrides.
# ---------------------------------------------------------------------------
for var in __EGL_VENDOR_LIBRARY_FILENAMES __EGL_VENDOR_LIBRARY_DIRS __GLX_VENDOR_LIBRARY_NAME \
           DRI_PRIME __NV_PRIME_RENDER_OFFLOAD __VK_LAYER_NV_optimus MESA_LOADER_DRIVER_OVERRIDE \
           LIBGL_ALWAYS_SOFTWARE GBM_BACKEND WEBKIT_DISABLE_DMABUF_RENDERER WEBKIT_DISABLE_COMPOSITING_MODE; do
  [ -n "${!var:-}" ] && die "GPU override $var is set; GUI tests run only with the desktop's own GPU selection"
done
[ -n "${DISPLAY:-}" ] || die "no DISPLAY: GUI tests need the real desktop session"
SESSION_DISPLAY="$(systemctl --user show-environment 2>/dev/null | sed -n 's/^DISPLAY=//p')"
if [ -n "$SESSION_DISPLAY" ] && [ "$DISPLAY" != "$SESSION_DISPLAY" ]; then
  die "DISPLAY=$DISPLAY is not the desktop session's display ($SESSION_DISPLAY)"
fi
display_number="${DISPLAY#*:}"; display_number="${display_number%%.*}"
if pgrep -a -x 'Xvfb|Xephyr|Xvnc|Xdummy' 2>/dev/null | grep -qE "(^| ):${display_number}( |$)"; then
  die "DISPLAY=$DISPLAY is served by a virtual X server"
fi

# One GUI test session at a time, across terminals and tools.
LOCK_FILE="${XDG_RUNTIME_DIR:-/tmp}/dalinira-gui-tests.lock"
exec 9>"$LOCK_FILE" || die "cannot open lock $LOCK_FILE"
flock -n 9 || die "another GUI test session holds $LOCK_FILE"

# Informational: which GPU drives which render node on this boot (numbering
# is not stable across boots, so it is never assumed).
for node in /sys/class/drm/renderD*; do
  [ -e "$node/device" ] || continue
  note "GPU $(basename "$node"): driver=$(basename "$(readlink -f "$node/device/driver")") vendor=$(cat "$node/device/vendor")"
done

PROBE="$BUILD_DIR/dalinira-x11-map-probe"
[ -x "$PROBE" ] || die "$PROBE is missing (build target dalinira-x11-map-probe)"
"$PROBE" 5000 || die "the desktop did not map a test window; GUI tests cannot run on this session"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
session_pids() {  # every process still in session $1
  ps -eo sid=,pid= | awk -v s="$1" '$1 == s { print $2 }'
}

wait_for_coredumps() {
  local waited=0
  while pgrep -x systemd-coredum >/dev/null 2>&1 || pgrep -f 'systemd-coredump@' >/dev/null 2>&1; do
    [ $waited -ge $COREDUMP_WAIT ] && die "core dump processing did not finish within ${COREDUMP_WAIT}s"
    sleep 1; waited=$((waited + 1))
  done
}

check_system_health() {  # $1 = epoch seconds when the test started
  local since="@$1" problems
  problems="$(journalctl -k --since "$since" -o short-precise --no-pager 2>/dev/null |
    grep -iE 'segfault|general protection|traps:|NVRM|Xid|nvidia.*(error|fail)|GPU HANG|gpu hang|i915.*(error|reset|hang)|amdgpu.*(error|fault|timeout)|drm.*(error|timeout)|xhci.*error|hung task|Out of memory')"
  if [ -n "$problems" ]; then
    echo "$problems" >&2
    die "kernel reported a crash or GPU/driver problem during the last test"
  fi
  # Listed even when core dumps are disabled (COREFILE "none").
  problems="$(coredumpctl list --since "$since" --no-legend --no-pager 2>/dev/null | grep -v 'No coredumps')"
  if [ -n "$problems" ]; then
    echo "$problems" >&2
    die "a process crashed during the last test"
  fi
  # The desktop itself: KWin logs this when its main thread was blocked.
  problems="$(journalctl --since "$since" --no-pager -o short-precise 2>/dev/null |
    grep -E 'main thread was hanging|kwin.*(frozen|not responding)')"
  if [ -n "$problems" ]; then
    echo "$problems" >&2
    die "the compositor reported a stall during the last test"
  fi
}

run_guarded() {  # $1 = label, $2 = hard timeout (s), rest = command
  local label="$1" limit="$2"; shift 2
  wait_for_coredumps
  local started; started="$(date +%s)"
  note "== $label (hard timeout ${limit}s)"
  # A new session per test: the test and every engine process it starts share
  # the session id, so the whole tree can be terminated together.
  setsid "$@" &
  local pid=$! status=0 elapsed=0 timed_out=0 stopped=""
  local log_base; log_base="$LOG_DIR/$(echo "$label" | tr -c 'A-Za-z0-9_.-' '_')-$(date +%Y%m%d-%H%M%S)"
  rm -f "$log_base.stop"
  python3 "$SCRIPT_DIR/gui-test-monitor.py" --session "$pid" --out "$log_base.csv" --stop-file "$log_base.stop" \
    > "$log_base.summary" 2>&1 &
  local monitor=$!
  while kill -0 "$pid" 2>/dev/null; do
    if [ -s "$log_base.stop" ]; then
      stopped="$(cat "$log_base.stop")"
      note "$label: resource limit: $stopped; terminating its process tree"
      kill -TERM -- "-$pid" 2>/dev/null; session_pids "$pid" | xargs -r kill -TERM 2>/dev/null
      sleep 3
      kill -KILL -- "-$pid" 2>/dev/null; session_pids "$pid" | xargs -r kill -KILL 2>/dev/null
      break
    fi
    if [ $elapsed -ge "$limit" ]; then
      timed_out=1
      note "$label exceeded ${limit}s; terminating its process tree"
      kill -TERM -- "-$pid" 2>/dev/null; session_pids "$pid" | xargs -r kill -TERM 2>/dev/null
      sleep 5
      kill -KILL -- "-$pid" 2>/dev/null; session_pids "$pid" | xargs -r kill -KILL 2>/dev/null
      break
    fi
    sleep 1; elapsed=$((elapsed + 1))
  done
  wait "$pid" 2>/dev/null; status=$?
  # Engine child processes (WebKit web/network, QtWebEngine) must follow the
  # test process out (their IPC connections close with it) before anything
  # else starts. Any that outlive it are orphans: terminate them and fail.
  local waited=0 orphans=""
  while [ -n "$(session_pids "$pid")" ]; do
    if [ $waited -ge $CLEANUP_WAIT ]; then
      orphans="$(session_pids "$pid" | xargs -r ps -o pid=,comm= -p 2>/dev/null | tr '\n' ' ')"
      note "$label left processes behind: $orphans; terminating them"
      session_pids "$pid" | xargs -r kill -KILL 2>/dev/null
      sleep 1
      break
    fi
    sleep 1; waited=$((waited + 1))
  done
  [ $waited -gt 0 ] && [ -z "$orphans" ] && note "$label: engine processes exited ${waited}s after the test"
  kill -TERM "$monitor" 2>/dev/null; wait "$monitor" 2>/dev/null
  sed 's/^/safe-gui-test: /' "$log_base.summary"
  wait_for_coredumps
  check_system_health "$started"
  [ -n "$stopped" ] && die "$label exceeded a resource limit: $stopped"
  [ -n "$orphans" ] && [ $timed_out = 0 ] && die "$label left orphan engine processes: $orphans"
  [ $timed_out = 1 ] && die "$label timed out"
  [ $status -ne 0 ] && die "$label failed (exit $status)"
  note "$label passed in ${elapsed}s"
  sleep 3   # let the compositor and GPU settle before the next GUI test
}

# ---------------------------------------------------------------------------
# Memory profiler (opt-in, one engine per invocation; uses live sites)
# ---------------------------------------------------------------------------
if [ -n "$PROFILE_ENGINE" ]; then
  case "$PROFILE_ENGINE" in chromium|webkit) ;; *) die "--memory-profile takes chromium or webkit" ;; esac
  [ -x "$BUILD_DIR/dalinira-memory-profile" ] || die "build target dalinira-memory-profile first"
  out="$BUILD_DIR/memory-profile-$PROFILE_ENGINE-$(date +%Y%m%d-%H%M%S).json"
  run_guarded "memory-profile ($PROFILE_ENGINE)" "$PROFILE_TIMEOUT" \
    env QT_QPA_PLATFORM=xcb GDK_BACKEND=x11 "$BUILD_DIR/dalinira-memory-profile" --engine "$PROFILE_ENGINE" --out "$out"
  note "profile written to $out"
  exit 0
fi

# ---------------------------------------------------------------------------
# GUI tests, strictly one at a time
# ---------------------------------------------------------------------------
[ ${#TESTS[@]} -eq 0 ] && mapfile -t TESTS < <(gui_tests)
DEFAULT_WEB_PROCESS_BUDGET="${DALINIRA_GUI_MAX_WEB_PROCESSES:-2}"
[ ${#TESTS[@]} -eq 0 ] && die "no GUI tests found (ctest label 'gui')"
LIVE_ENV=()
[ "$LIVE" = 1 ] && LIVE_ENV=(DALINIRA_LIVE_TESTS=1 DALINIRA_LIVE_MEDIA_TEST=1)

for name in "${TESTS[@]}"; do
  limit="$(test_timeout "$name")"
  [ "${limit:-0}" -gt 0 ] || limit=$DEFAULT_TIMEOUT
  # ctest enforces its own TIMEOUT first; this is the outer hard limit.
  budget="$(test_web_process_budget "$name")"
  [ -n "$budget" ] && note "$name declares a budget of $budget WebKit web processes"
  export DALINIRA_GUI_MAX_WEB_PROCESSES="${budget:-$DEFAULT_WEB_PROCESS_BUDGET}"
  run_guarded "$name" $((limit + 30)) \
    env "${LIVE_ENV[@]}" ctest --test-dir "$BUILD_DIR" -R "^${name}\$" --output-on-failure --timeout "$limit"
done
note "all ${#TESTS[@]} GUI test(s) passed; no crashes or GPU errors were logged"
