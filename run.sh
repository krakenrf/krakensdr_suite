#!/usr/bin/env bash
#
# run.sh - Start the full KrakenSDR v2 stack in a split-screen terminal.
#
#   * Top pane:    Heimdall server (its live TUI dashboard)
#   * Bottom pane: waits for Heimdall phase convergence, then launches the
#                  KrakenSDR DoA client (its live TUI dashboard)
#
# Both apps keep their native terminal dashboards; tmux gives each one a real
# pty so the dashboards render. Convergence is detected out-of-band by reading
# the phase_state field from Heimdall's data port (8091), so it works even while
# the dashboard owns Heimdall's stdout.
#
# Controls (inside tmux):
#   Ctrl-b  then  arrow keys   switch panes
#   Ctrl-b  then  z            zoom/unzoom the focused pane (fullscreen it)
#   Ctrl-b  then  d            detach (apps keep running in the background)
#
# Usage:
#   ./run.sh                    # split-screen; wait for convergence, then client
#   ./run.sh stop               # stop everything (the tmux session and any
#                               # stale heimdall/kraken_doa left outside it)
#   ./run.sh --wideband         # KrakenSDR Wideband variant (passes --wideband
#                               # to both apps; WIDEBAND=1 ./run.sh also works)
#   ./run.sh --kerberos         # KerberosSDR: manual calibration only - the
#                               # client starts immediately (no convergence
#                               # wait; calibrate from the heimdall web UI at
#                               # :8070). KERBEROS=1 ./run.sh also works
#   ./run.sh --kerberos_sw      # KerberosSDR with CKOVAL antenna switches:
#                               # automatic calibration, normal convergence
#                               # wait. KERBEROS_SW=1 ./run.sh also works
#   ./run.sh --ext_noise        # KrakenSDR + add-on antenna array with its own
#                               # noise source on the CH0 bias tee (heimdall
#                               # switches that instead of the internal noise
#                               # source). EXT_NOISE=1 ./run.sh also works
#   WAIT_TIMEOUT=600 ./run.sh   # allow up to 600s for convergence (default 300)
#   WAIT_TIMEOUT=0 ./run.sh     # wait forever for convergence
#   NO_AUTOBUILD=1 ./run.sh     # don't build: an app that isn't built (or was
#                               # built for another CPU / before a pull) is
#                               # otherwise built first (scripts/autobuild.sh,
#                               # progress page on :8070 / :8080; JOBS=N sets
#                               # its parallel jobs, default by RAM, 1-3)
#   NO_TMUX=1 ./run.sh          # headless: no dashboards, log to files instead
#
# Test knobs: BUILD_PAGE_HTTP_PORT / BUILD_PAGE_HTTPS_PORT move the build
# progress page off 8070 / 8080 (a test copy next to a live stack).
#
set -uo pipefail

# Resolve the directory this script lives in so it works from anywhere.
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname "$SELF")"
HEIMDALL_DIR="$SCRIPT_DIR/heimdall_v2"
KRAKEN_DIR="$SCRIPT_DIR/kraken_doa_v2"
LOG_DIR="$SCRIPT_DIR/logs"

WAIT_TIMEOUT="${WAIT_TIMEOUT:-300}"     # seconds to wait for convergence; 0 = forever
# The exit status that means "start me again": the web page's ↻ Restart
# software makes heimdall and kraken_doa exit with it. Passed to both apps as
# KRAKEN_RESTART_EXIT (they offer / accept the restart only when it is set);
# the supervisor and the headless loop restart on it
# (kraken_doa_v2/docs/dev/power.md)
RESTART_EXIT=75
SESSION="${TMUX_SESSION:-krakensdr}"    # tmux session name
DATA_PORT=8091                          # Heimdall TCP data port (carries phase_state)

# Automatic build (scripts/autobuild.sh) and its progress page
# (scripts/build_page.py) - see "Automatic build" below
AUTOBUILD="$SCRIPT_DIR/scripts/autobuild.sh"
BUILD_PAGE="$SCRIPT_DIR/scripts/build_page.py"
BUILD_STATUS="$LOG_DIR/build_status.json"
BUILD_LOG="$LOG_DIR/build.log"
BUILD_PAGE_PID="$LOG_DIR/build_page.pid"
BUILD_PAGE_LOG="$LOG_DIR/build_page.log"
BUILD_PAGE_HTTP_PORT="${BUILD_PAGE_HTTP_PORT:-8070}"     # heimdall's web UI port
BUILD_PAGE_HTTPS_PORT="${BUILD_PAGE_HTTPS_PORT:-8080}"   # kraken_doa's

# Hardware-variant flags (combinable, order-independent):
#   --wideband / -w  KrakenSDR Wideband variant (passed to BOTH apps)
#   --kerberos       KerberosSDR, manual calibration (heimdall only; the
#                    client auto-detects the mode from the data stream)
#   --kerberos_sw    KerberosSDR with CKOVAL antenna switches (heimdall only)
#   --ext_noise      add-on array with its own noise source, powered from the
#                    CH0 bias tee (heimdall only; not with --wideband/--kerberos)
# Matching env vars (WIDEBAND=1 / KERBEROS=1 / KERBEROS_SW=1 / EXT_NOISE=1) also work and
# are how the flags survive into the tmux client pane re-invocation.
WIDEBAND="${WIDEBAND:-0}"
KERBEROS="${KERBEROS:-0}"
KERBEROS_SW="${KERBEROS_SW:-0}"
EXT_NOISE="${EXT_NOISE:-0}"
# Only LEADING flags are parsed: the internal entrypoints (__supervise,
# __client_pane) carry the app's own argv after them, which must not be eaten.
# Whatever is left is validated below, once die() exists.
while (( $# )); do
    case "$1" in
        --wideband|-w)                WIDEBAND=1 ;;
        --kerberos)                   KERBEROS=1 ;;
        --kerberos_sw|--kerberos-sw)  KERBEROS_SW=1 ;;
        --ext_noise|--ext-noise)      EXT_NOISE=1 ;;
        *) break ;;
    esac
    shift
done
WB_FLAG=""
[[ "$WIDEBAND" == "1" ]] && WB_FLAG="--wideband"
# --kerberos_sw implies (and supersedes) --kerberos on the heimdall side.
KB_FLAG=""
[[ "$KERBEROS" == "1" ]] && KB_FLAG="--kerberos"
[[ "$KERBEROS_SW" == "1" ]] && KB_FLAG="--kerberos_sw"
EN_FLAG=""
[[ "$EXT_NOISE" == "1" ]] && EN_FLAG="--ext_noise"
# Manual-calibration mode: heimdall never converges on its own (the user must
# recalibrate from its web UI), so waiting for convergence would just burn the
# whole timeout - start the client immediately instead.
KB_MANUAL=0
[[ "$KERBEROS" == "1" && "$KERBEROS_SW" != "1" ]] && KB_MANUAL=1

# --- pretty output ----------------------------------------------------------
if [[ -t 1 ]]; then
    BOLD=$'\033[1m'; GRN=$'\033[32m'; YEL=$'\033[33m'; RED=$'\033[31m'; CYN=$'\033[36m'; RST=$'\033[0m'
else
    BOLD=""; GRN=""; YEL=""; RED=""; CYN=""; RST=""
fi
say()  { echo "${CYN}${BOLD}==>${RST} ${BOLD}$*${RST}"; }
ok()   { echo "${GRN}   ✓ $*${RST}"; }
warn() { echo "${YEL}   ! $*${RST}"; }
die()  { echo "${RED}${BOLD}Error:${RST} $*" >&2; exit 1; }
usage() { sed -n '/^# Usage:/,/^# *NO_TMUX/s/^# \{0,1\}//p' "$SELF"; }

# Reject anything unrecognised. A mistyped flag used to be ignored silently,
# so e.g. a misspelled --kerberos_sw started a switchless KerberosSDR in
# automatic calibration - calibrating against the live antennas.
case "${1:-}" in
    ""|stop|__supervise|__client_pane|__build_pane) ;;
    -h|--help) usage; exit 0 ;;
    *) echo "${RED}${BOLD}Error:${RST} unknown option '$1'" >&2; usage >&2; exit 2 ;;
esac
# heimdall refuses this combination too - fail here, before anything is stopped
[[ "$EXT_NOISE" == "1" && ( "$WIDEBAND" == "1" || -n "$KB_FLAG" ) ]] &&
    die "--ext_noise cannot be combined with --wideband or --kerberos(_sw)"

# ===========================================================================
# Convergence probe: poll Heimdall's data port until phase_state == CONVERGED.
# The wire header (see heimdall_v2/src/net/tcp_data_server.cpp) is big-endian:
#   magic(4) 'MCHQ' | num_channels(4) | num_samples(4) | phase_state(4) | ...
# CONVERGED == 4 (PhaseCompensatorState enum). Each fresh connection starts on a
# packet boundary, so reading the first 16 bytes yields a valid header.
# Exit 0 = converged, 1 = timed out, 2 = the watched process exited,
# 3 = heimdall runs in wideband scan / independent mode (phase-state bits
# 0x400 / 0x800): no calibration runs there, so there is nothing to wait for
# (waiting used to hold the client back until the timeout - and headless mode
# then gave up on the whole stack).
# arg1 = timeout seconds (0 = forever); arg2 = pid whose exit means Heimdall
# is gone for good (0 = don't watch) - otherwise the wait would poll a dead
# port until the timeout, or forever with WAIT_TIMEOUT=0.
# ===========================================================================
wait_for_convergence() {
    python3 - "$1" "$DATA_PORT" "${2:-0}" <<'PY'
import os, socket, struct, sys, time
timeout = float(sys.argv[1]); port = int(sys.argv[2]); watch = int(sys.argv[3])
def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    return True
MAGIC = 0x4D434851; CONVERGED = 4
NAMES = {0:"WAIT-LAG",1:"MEASURING",2:"APPLYING",3:"VERIFYING",
         4:"CONVERGED",5:"COOLDOWN",6:"PER-BIN"}
start = time.time()
while True:
    el = int(time.time() - start)
    if timeout > 0 and (time.time() - start) > timeout:
        sys.stderr.write("\n"); sys.exit(1)
    if watch > 0 and not alive(watch):
        sys.stderr.write("\n"); sys.exit(2)
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        buf = b""
        while len(buf) < 16:
            c = s.recv(16 - len(buf))
            if not c:
                break
            buf += c
        s.close()
        if len(buf) >= 16:
            magic, ch, samp, phase = struct.unpack(">IIII", buf[:16])
            if magic == MAGIC and phase & 0xC00:
                sys.stderr.write("\r   Heimdall is in %s mode - no calibration to wait for\n"
                                 % ("independent" if phase & 0x800 else "wideband scan"))
                sys.exit(3)
            phase &= 0xFF  # high bits carry mode / KerberosSDR flags; low byte is the state
            if magic == MAGIC:
                sys.stderr.write("\r   [%3ds] Heimdall phase: %-10s" % (el, NAMES.get(phase, str(phase))))
                sys.stderr.flush()
                if phase == CONVERGED:
                    sys.stderr.write("\n"); sys.exit(0)
    except OSError:
        sys.stderr.write("\r   [%3ds] waiting for Heimdall data port %d..." % (el, port))
        sys.stderr.flush()
    time.sleep(1)
PY
}

# ===========================================================================
# Supervisor: run an app in the FOREGROUND of its pane (so its dashboard keeps
# the terminal) and restart it if it dies. A crash leaves only a dead pane
# otherwise - and with tmux as the systemd unit's main process, the unit's
# Restart=on-failure never sees it, so an unattended Pi stayed down.
#   exit 0 (clean shutdown)            -> done, no restart
#   SIGTERM/SIGINT/SIGHUP to the pane  -> the app shuts down cleanly, done
#   exit RESTART_EXIT (web page ↻)     -> restart after 1 s
#   any other exit (crash, init error) -> restart after RESTART_DELAY seconds
# The app runs as a background child that we `wait` on: bash defers a trap
# until a FOREGROUND child returns, and closing a tmux pane/window (or the
# server exiting) hangs up the pty, which SIGHUPs only this shell - the pane's
# session leader - not the app. The trap therefore forwards the signal to the
# app and keeps waiting through its clean shutdown. (Without job control the
# child stays in the pane's foreground process group, so Ctrl+C and the
# dashboard behave as before; `<&0` keeps stdin, which a background job would
# otherwise get as /dev/null.)
# ===========================================================================
supervise() {  # supervise <name> <command...>
    local name="$1"; shift
    local stopping=0 rc child=
    trap 'stopping=1; [[ -n "$child" ]] && kill -TERM "$child" 2>/dev/null' TERM INT HUP
    while :; do
        KRAKEN_RESTART_EXIT="$RESTART_EXIT" "$@" <&0 &
        child=$!
        # wait returns early (status > 128) when a trap fires; keep waiting
        # until the app itself has exited
        while :; do
            wait "$child"; rc=$?
            kill -0 "$child" 2>/dev/null || break
        done
        child=
        [[ "$stopping" == "1" || "$rc" == "0" ]] && return "$rc"
        if [[ "$rc" == "$RESTART_EXIT" ]]; then
            ok "$name: restart requested from the web page - starting it again"
            sleep 1
        else
            warn "$name exited (status $rc) - restarting in ${RESTART_DELAY:-5}s (Ctrl+C to stay stopped)"
            sleep "${RESTART_DELAY:-5}"
        fi
        [[ "$stopping" == "1" ]] && return "$rc"
    done
}

if [[ "${1:-}" == "__supervise" ]]; then
    shift
    supervise "$@"
    exit $?
fi

# ===========================================================================
# Internal entrypoint run inside the tmux BOTTOM pane: wait, then run client.
# ===========================================================================
if [[ "${1:-}" == "__client_pane" ]]; then
    if [[ "$KB_MANUAL" == "1" ]]; then
        warn "KerberosSDR manual-calibration mode: starting the client immediately."
        warn "DoA output is INVALID until you calibrate: disconnect all antennas,"
        warn "then press 'Full Recalibration' in the heimdall web UI (:8070)."
        sleep 2
    else
        echo "${BOLD}Waiting for Heimdall phase convergence"\
"$([[ "$WAIT_TIMEOUT" -eq 0 ]] && echo " (no timeout)" || echo " (timeout ${WAIT_TIMEOUT}s)")...${RST}"
        wait_for_convergence "$WAIT_TIMEOUT" "${HEIMDALL_PID:-0}"
        case $? in
        0)
            ok "Phase converged - DF output is valid. Starting client..."
            sleep 1
            ;;
        3)
            ok "Starting client..."
            ;;
        2)
            # The top pane's supervisor ended: heimdall exited cleanly or was
            # stopped, and will not come back on its own.
            warn "Heimdall has stopped (see the top pane) - not starting the client."
            warn "Start the stack again with: $SELF"
            exit 1
            ;;
        *)
            warn "No convergence within ${WAIT_TIMEOUT}s. Starting client anyway"
            warn "(DoA output stays invalid until Heimdall converges - watch the top pane)."
            sleep 3
            ;;
        esac
    fi
    cd "$KRAKEN_DIR" || die "cannot cd to $KRAKEN_DIR"
    supervise "KrakenSDR DoA client" ./kraken_doa $WB_FLAG
    exit $?
fi

# ===========================================================================
# Graceful session stop: SIGTERM every pane's process group (the supervisor
# and its app), wait for the apps' clean shutdown - heimdall releases the
# dongles and restores the KerberosSDR switch GPIOs - then remove the session.
# Killing the session outright gave the apps no time to finish, so a relaunch
# could race the old heimdall for the USB devices.
# ===========================================================================
# Only the panes run.sh started (supervisor / client pane): a shell the user
# opened in the session (Ctrl-b c) ignores SIGTERM, which held this for the
# full 10 s. `keep` leaves the session itself open (see inside_session).
stop_session() {
    local pids pid alive
    # live panes only: a dead one's pid (remain-on-exit) may be reused - or a
    # zombie tmux hasn't reaped yet (seen after a failed build), which held this 10 s
    pids=$(tmux list-panes -s -t "$SESSION" -F '#{pane_pid} #{pane_dead} #{pane_start_command}' 2>/dev/null |
           awk '$2 == "0" && /__supervise|__client_pane|__build_pane/ {print $1}')
    for pid in $pids; do
        kill -TERM -- "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
    done
    for _ in $(seq 1 50); do            # up to 10 s
        alive=0
        for pid in $pids; do kill -0 "$pid" 2>/dev/null && alive=1; done
        [[ "$alive" == "0" ]] && break
        sleep 0.2
    done
    [[ "${alive:-0}" == "1" ]] && warn "Apps still running after 10s - forcing the session closed."
    [[ "${1:-}" == "keep" ]] || tmux kill-session -t "$SESSION" 2>/dev/null || true
}

# True when this script runs inside the session it manages (e.g. a shell the
# user opened there): closing that session closes our own pane, and the
# SIGHUP killed run.sh before it got any further.
inside_session() {
    [[ -n "${TMUX:-}" ]] && [[ "$(tmux display-message -p '#S' 2>/dev/null)" == "$SESSION" ]]
}

# ===========================================================================
# Stale-process sweep: stop heimdall / kraken_doa and run.sh supervisors left
# over from an earlier run that no tmux session owns any more (a tmux server
# that died, an older run.sh whose supervisor ignored the pane closing, a
# headless run that lost its terminal, an app started by hand). They hold the
# dongles and ports, so a new stack would fail to start. Each is stopped via
# its process group (supervisor + app + convergence probe): SIGTERM for the
# apps' clean shutdown, SIGKILL for anything still alive after 10 s.
# Processes of other users (e.g. started with sudo) can't be signalled - they
# are reported instead.
# ===========================================================================
stop_stale() {
    local uid my_pgid pids foreign pid pgid pgids="" alive
    uid=$(id -u)
    my_pgid=$(ps -o pgid= -p $$ | tr -d ' ')
    pids=$( { pgrep -u "$uid" -x heimdall; pgrep -u "$uid" -x kraken_doa
              pgrep -u "$uid" -f -- "$SELF __(supervise|client_pane|build_pane)"
              # a headless run's build (its group = that run.sh): two makes in one tree
              pgrep -u "$uid" -f -- "$AUTOBUILD build"; } 2>/dev/null | sort -un)
    for pid in $pids; do
        pgid=$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')
        # our own process group is this run, never stale
        [[ -z "$pgid" || "$pgid" == "$my_pgid" ]] && continue
        [[ " $pgids " == *" $pgid "* ]] || pgids+=" $pgid"
    done
    pgids="${pgids# }"

    foreign=$( { pgrep -x heimdall; pgrep -x kraken_doa; } 2>/dev/null |
               while read -r pid; do
                   [[ "$(ps -o uid= -p "$pid" | tr -d ' ')" != "$uid" ]] &&
                       ps -o pid=,user=,comm= -p "$pid"
               done)
    [[ -n "$foreign" ]] && warn "Running as another user (stop with sudo kill):" &&
        echo "$foreign" | sed 's/^ */     /'

    [[ -z "$pgids" ]] && return 0
    warn "Stopping stale processes from an earlier run:"
    # pgrep -g selects by process GROUP (ps -g means session id)
    ps -o pid=,etime=,args= -p "$(pgrep -d, -g "${pgids// /,}")" 2>/dev/null | sed 's/^ */     /'
    for pgid in $pgids; do kill -TERM -- "-$pgid" 2>/dev/null; done
    for _ in $(seq 1 50); do            # up to 10 s for the clean shutdown
        alive=0
        for pgid in $pgids; do pgrep -g "$pgid" >/dev/null && alive=1; done
        [[ "$alive" == "0" ]] && break
        sleep 0.2
    done
    if [[ "$alive" == "1" ]]; then
        warn "Still running after 10s - force-killing."
        for pgid in $pgids; do kill -KILL -- "-$pgid" 2>/dev/null; done
        sleep 0.5
    fi
    ok "Stale processes stopped."
}

# ===========================================================================
# Automatic build. When scripts/autobuild.sh check says an app isn't built
# for this machine (missing, another CPU's flags stamp, sources newer than
# the build - e.g. after a git pull), it is built before the stack starts:
# after the old stack is stopped (the ports are free and no running binary is
# relinked), with scripts/build_page.py serving a progress page on the web UI
# ports (8070 / 8080) that reloads into the real UI once that answers. In
# tmux mode the build runs in the session's first window (__build_pane),
# which then opens the stack's window, so run.sh itself returns at once as
# before (the boot service's Type=forking start isn't held up by a long
# build). If the build fails, an older build that was made for this CPU is
# started anyway (loud warning); otherwise the page keeps showing the error
# (detached, pidfile logs/build_page.pid) until `run.sh stop` or the next
# start. docs/dev/run-and-deploy.md has the details.
# ===========================================================================
build_page_pid_ok() {   # the pid is our page server
    [[ -n "${1:-}" ]] && ps -o args= -p "$1" 2>/dev/null | grep -q "build_page\.py"
}
stop_build_page() {
    local pid="" pids="" p alive=0
    [[ -f "$BUILD_PAGE_PID" ]] && pid="$(head -n 1 "$BUILD_PAGE_PID" 2>/dev/null | tr -dc '0-9')"
    build_page_pid_ok "$pid" && pids="$pid"
    # one whose pidfile isn't written yet (still binding) or is gone
    # (with its first argument: not an editor or a `less` that has the file open)
    for p in $(pgrep -u "$(id -u)" -f -- "$BUILD_PAGE --status" 2>/dev/null); do
        [[ " $pids " == *" $p "* ]] || pids="$pids $p"
    done
    pids="${pids# }"
    rm -f "$BUILD_PAGE_PID"
    [[ -z "$pids" ]] && return 0
    for p in $pids; do kill -TERM "$p" 2>/dev/null; done
    for _ in $(seq 1 25); do            # up to 5 s (it closes its ports in ~0.5 s)
        alive=0
        for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done
        [[ "$alive" == "0" ]] && break
        sleep 0.2
    done
    [[ "$alive" == "1" ]] && for p in $pids; do kill -KILL "$p" 2>/dev/null; done
    return 0
}

# A first status before the page server reads it: a stale "done" / "failed"
# from an earlier build would otherwise show (or reload the page) at once
write_build_status() {
    local now tmp="$BUILD_STATUS.tmp"
    now="$(date +%s)"
    printf '{"state":"building","percent":0,"phase":"preparing","step":"checking what to build","done":0,"total":0,"started":%s,"updated":%s,"log_tail":[]}\n' \
        "$now" "$now" > "$tmp" && mv -f "$tmp" "$BUILD_STATUS"
}

# Start the page server detached (its own session: it outlives run.sh when a
# failed build leaves it up, and a terminal hang-up doesn't reach it).
# 0 = serving (pidfile written once a port is bound), 1 = not (port busy, no
# python ssl, ... - the build goes on with terminal output only)
start_build_page() {
    local pid i
    if [[ ! -f "$BUILD_PAGE" ]]; then
        warn "No ${BUILD_PAGE#$SCRIPT_DIR/} - building without the progress page."
        return 1
    fi
    rm -f "$BUILD_PAGE_PID"
    python3 -c 'import os, sys
try:
    os.setsid()
except OSError:
    pass
os.execvp(sys.argv[1], sys.argv[1:])' python3 "$BUILD_PAGE" --status "$BUILD_STATUS" \
        --http "$BUILD_PAGE_HTTP_PORT" --https "$BUILD_PAGE_HTTPS_PORT" \
        --cert "$KRAKEN_DIR/server.crt" --key "$KRAKEN_DIR/server.key" \
        --pidfile "$BUILD_PAGE_PID" > "$BUILD_PAGE_LOG" 2>&1 < /dev/null &
    pid=$!
    for i in $(seq 1 75); do            # up to 15 s: a Pi may make the TLS certificate first
        [[ -s "$BUILD_PAGE_PID" ]] && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$pid" 2>/dev/null && return 0   # still starting: let it be
    warn "The build progress page did not start (log: ${BUILD_PAGE_LOG#$SCRIPT_DIR/}):"
    tail -n 3 "$BUILD_PAGE_LOG" 2>/dev/null | sed 's/^/     /'
    warn "Building with terminal output only."
    return 1
}

build_error() {   # the one-line error of the failed build
    python3 -c 'import json, sys
try:
    print(json.load(open(sys.argv[1])).get("error") or "")
except Exception:
    pass' "$BUILD_STATUS" 2>/dev/null
}

# Build, with the page up. 0 = start the apps (built, or an older build for
# this CPU is used after a failure), 1 = nothing to run (error printed, the
# page left up showing it)
build_child=""
build_interrupted() {
    trap - INT TERM HUP
    [[ -n "$build_child" ]] && kill -TERM "$build_child" 2>/dev/null
    [[ -n "$build_child" ]] && wait "$build_child" 2>/dev/null
    stop_build_page
    echo
    warn "Build interrupted."
    exit 130
}
build_and_report() {
    local h_old=0 k_old=0 page=0 rc err
    # What could still run if the build fails - decided BEFORE it, as the
    # build rewrites the flags stamps before it relinks anything
    "$AUTOBUILD" runnable heimdall >/dev/null 2>&1 && h_old=1
    "$AUTOBUILD" runnable kraken_doa >/dev/null 2>&1 && k_old=1
    mkdir -p "$LOG_DIR"
    write_build_status
    if start_build_page; then
        page=1
        ok "Build progress page: http://localhost:${BUILD_PAGE_HTTP_PORT}  https://localhost:${BUILD_PAGE_HTTPS_PORT}"
    fi
    say "Building (log: ${BUILD_LOG#$SCRIPT_DIR/}) - the first build takes 10-30 min on a Raspberry Pi"
    trap build_interrupted INT TERM HUP
    # in the background + wait, so a TERM to this shell is handled at once
    "$AUTOBUILD" build --status "$BUILD_STATUS" --log "$BUILD_LOG" &
    build_child=$!
    while :; do
        wait "$build_child"; rc=$?
        kill -0 "$build_child" 2>/dev/null || break
    done
    build_child=""
    trap - INT TERM HUP
    if [[ "$rc" == "0" ]]; then
        ok "Build complete."
        # a moment for the page to see "done" before its server goes away
        [[ "$page" == "1" ]] && sleep 2
        stop_build_page
        return 0
    fi
    err="$(build_error)"
    # an app is fine when it was built now, or its older build was made for this
    # CPU and is still there (a failed link deletes the executable)
    if { "$AUTOBUILD" check heimdall >/dev/null 2>&1 || [[ "$h_old" == "1" && -x "$HEIMDALL_DIR/heimdall" ]]; } &&
       { "$AUTOBUILD" check kraken_doa >/dev/null 2>&1 || [[ "$k_old" == "1" && -x "$KRAKEN_DIR/kraken_doa" ]]; }; then
        echo "${RED}${BOLD}" >&2
        echo "  !!! THE BUILD FAILED - starting the OLDER build that is still here !!!" >&2
        echo "  ${err:-see ${BUILD_LOG}}" >&2
        echo "  Fix it (./install.sh for missing dependencies, a network for the first" >&2
        echo "  fetch), then run ./run.sh again. Full log: ${BUILD_LOG}${RST}" >&2
        echo >&2
        stop_build_page
        sleep 3
        return 0
    fi
    echo "${RED}${BOLD}Error:${RST} the build failed and there is no working build to start:" >&2
    echo "  ${err:-see ${BUILD_LOG}}" >&2
    echo "  Full log: ${BUILD_LOG}" >&2
    if [[ "$page" == "1" ]]; then
        echo "  The progress page keeps showing this error (on :${BUILD_PAGE_HTTP_PORT} / :${BUILD_PAGE_HTTPS_PORT}) until '$SELF stop' or the next start." >&2
    fi
    return 1
}

# ===========================================================================
# Split-screen launch: heimdall in the top pane (supervised), the
# convergence wait + client below. `window` = from the build pane: a new
# window in the existing session instead of a new session.
# ===========================================================================
launch_tmux_stack() {
    local cmd="exec '$SELF' __supervise Heimdall ./heimdall $WB_FLAG $KB_FLAG $EN_FLAG"
    # Top pane: Heimdall with its live dashboard (real pty -> TUI renders).
    # Supervised: restarted if it crashes (see supervise).
    if [[ "${1:-}" == "window" ]]; then
        HEIMDALL_PID=$(tmux new-window -d -P -F '#{pane_pid}' -t "$SESSION:" -n krakensdr -c "$HEIMDALL_DIR" "$cmd")
    else
        HEIMDALL_PID=$(tmux new-session -d -P -F '#{pane_pid}' -s "$SESSION" -n krakensdr -c "$HEIMDALL_DIR" "$cmd")
    fi

    # Keep dead panes visible so a crash leaves its message on screen for diagnosis.
    # (remain-on-exit is a WINDOW option -> set with -w; do it before anything can exit.)
    tmux set-option -w -t "$SESSION:krakensdr" remain-on-exit on
    tmux set-option -t "$SESSION" mouse on

    # Bottom pane: wait-for-convergence, then the client. Re-invokes this script's
    # internal entrypoint; WAIT_TIMEOUT, the variant flags and the heimdall pane's
    # supervisor pid (its exit ends the wait) are passed through as env vars.
    tmux split-window -v -t "$SESSION:krakensdr" -c "$KRAKEN_DIR" \
        "exec env WAIT_TIMEOUT='$WAIT_TIMEOUT' HEIMDALL_PID='$HEIMDALL_PID' WIDEBAND='$WIDEBAND' KERBEROS='$KERBEROS' KERBEROS_SW='$KERBEROS_SW' '$SELF' __client_pane"

    tmux select-pane -t "$SESSION:krakensdr.0"
    [[ "${1:-}" == "window" ]] && tmux select-window -t "$SESSION:krakensdr"
    return 0
}

# ===========================================================================
# Internal entrypoint: the tmux build window. Builds, then opens the stack's
# window and closes itself; after a failure with nothing to start, the pane
# stays (remain-on-exit) with the error on screen and the session alive.
# ===========================================================================
if [[ "${1:-}" == "__build_pane" ]]; then
    if build_and_report; then
        say "Starting the stack (window 'krakensdr')..."
        launch_tmux_stack window
        exit 0
    fi
    tmux set-option -w -t "${TMUX_PANE:-$SESSION:build}" remain-on-exit on 2>/dev/null
    exit 1
fi

# ===========================================================================
# `stop` subcommand: stop both apps cleanly and tear down the tmux session.
# ===========================================================================
if [[ "${1:-}" == "stop" ]]; then
    if command -v tmux >/dev/null 2>&1 && tmux has-session -t "$SESSION" 2>/dev/null; then
        if inside_session; then
            # Everything else first: closing the session ends this shell too
            stop_session keep
            stop_stale
            stop_build_page
            ok "Stopped the stack - closing tmux session '$SESSION' (this window too)."
            tmux kill-session -t "$SESSION" 2>/dev/null
            exit 0
        fi
        stop_session
        ok "Stopped tmux session '$SESSION'."
    else
        warn "No running '$SESSION' session found."
    fi
    stop_stale
    # a build progress page left up by a failed build
    stop_build_page
    exit 0
fi

# --- sanity checks ----------------------------------------------------------
command -v python3 >/dev/null 2>&1 || die "python3 is required for convergence detection."
HEADLESS=0
if [[ "${NO_TMUX:-0}" == "1" ]] || ! command -v tmux >/dev/null 2>&1; then HEADLESS=1; fi
# Built for this machine? (fast and read-only: flags stamps + file times)
NEED_BUILD=0
if [[ "${NO_AUTOBUILD:-0}" == "1" || ! -x "$AUTOBUILD" ]]; then
    [[ -x "$HEIMDALL_DIR/heimdall" ]] || die "heimdall not built. Run ${BOLD}${SCRIPT_DIR}/install.sh${RST} first."
    [[ -x "$KRAKEN_DIR/kraken_doa" ]] || die "kraken_doa not built. Run ${BOLD}${SCRIPT_DIR}/install.sh${RST} first."
elif ! build_check="$("$AUTOBUILD" check 2>&1)"; then
    NEED_BUILD=1
    say "A build is needed first:"
    echo "$build_check" | grep -v ': up to date$' | sed 's/^/     /'
fi

# Clear out a previous stack before starting a new one: a live session is
# stopped cleanly, then anything orphaned outside it (see stop_stale).
if command -v tmux >/dev/null 2>&1 && tmux has-session -t "$SESSION" 2>/dev/null; then
    # Replacing the session from inside it would close this very shell
    # mid-start (the old stack stopped, the new one never started)
    inside_session && die "run.sh is running inside the '$SESSION' tmux session it would replace - detach (Ctrl-b d) or use another terminal, then run it again."
    warn "A '$SESSION' session is already running - replacing it."
    stop_session
fi
stop_stale
stop_build_page   # one left up by an earlier failed build: it holds the web ports

# ===========================================================================
# Headless fallback (NO_TMUX=1 or tmux missing): no dashboards; log to files.
# ===========================================================================
run_headless() {
    local hlog="$LOG_DIR/heimdall.log" klog="$LOG_DIR/kraken_doa.log"
    # NOTE: hpid/kpid are intentionally NOT 'local' - the EXIT-trap cleanup runs
    # after this function returns (e.g. when Heimdall dies) and must still see
    # them to kill the client. As locals they would be out of scope by then.
    hpid=""; kpid=""; tpid=""; stopping=0
    mkdir -p "$LOG_DIR"
    cleanup() {
        stopping=1  # a requested stop: heimdall ending now is not a crash
        trap - INT TERM EXIT; echo
        say "Shutting down..."
        [[ -n "$kpid" ]] && kill "$kpid" 2>/dev/null || true
        [[ -n "$hpid" ]] && kill "$hpid" 2>/dev/null || true
        # the log follower (macOS: it doesn't end with heimdall by itself)
        [[ -n "$tpid" ]] && kill "$tpid" 2>/dev/null || true
        # Grace period, then hard-kill any straggler so a child that ignores
        # SIGTERM can't hang shutdown (SIGKILL can't be trapped -> wait returns).
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            { [[ -z "$kpid" ]] || ! kill -0 "$kpid" 2>/dev/null; } &&
            { [[ -z "$hpid" ]] || ! kill -0 "$hpid" 2>/dev/null; } && break
            sleep 0.3
        done
        [[ -n "$kpid" ]] && kill -9 "$kpid" 2>/dev/null || true
        [[ -n "$hpid" ]] && kill -9 "$hpid" 2>/dev/null || true
        wait 2>/dev/null || true
        ok "Stopped."
    }
    trap cleanup INT TERM EXIT

    say "Starting Heimdall server (headless; log: ${hlog#$SCRIPT_DIR/})"
    # a restart from the web page (see the loop below) keeps the logs
    [[ "${HEADLESS_RESTARTED:-0}" == "1" ]] || : >"$hlog"
    [[ "${HEADLESS_RESTARTED:-0}" == "1" ]] || : >"$klog"
    ( cd "$HEIMDALL_DIR" && HEIMDALL_NO_TUI=1 KRAKEN_RESTART_EXIT="$RESTART_EXIT" exec ./heimdall $WB_FLAG $KB_FLAG $EN_FLAG ) >>"$hlog" 2>&1 &
    hpid=$!
    echo "     Web UI: http://localhost:8070"

    if [[ "$KB_MANUAL" == "1" ]]; then
        warn "KerberosSDR manual-calibration mode: skipping the convergence wait."
        warn "DoA output is INVALID until you calibrate: disconnect all antennas,"
        warn "then press 'Full Recalibration' in the heimdall web UI (:8070)."
        sleep 2
    else
        say "Waiting for phase convergence$([[ "$WAIT_TIMEOUT" -eq 0 ]] && echo " (no timeout)" || echo " (timeout ${WAIT_TIMEOUT}s)")..."
        wait_for_convergence "$WAIT_TIMEOUT" "$hpid"
        local wrc=$?
        # Ctrl+C / TERM during the wait: the trap's cleanup already stopped
        # everything - a requested stop, not a timeout (this used to fall into
        # the "Timed out" branch below and exit 1).
        [[ "$stopping" == "1" ]] && return 0
        case $wrc in
        0) ok "Phase converged - DF output is valid." ;;
        3) ok "No calibration in this mode - starting the client." ;;
        2)
            warn "Heimdall exited during startup. Last log lines:"
            tail -n 15 "$hlog" | sed 's/^/     /'
            die "Heimdall failed to start (see ${hlog#$SCRIPT_DIR/})."
            ;;
        *)
            warn "No convergence in time. Last Heimdall log lines:"
            tail -n 15 "$hlog" | sed 's/^/     /'
            die "Timed out (check antennas/noise source, or raise WAIT_TIMEOUT)."
            ;;
        esac
    fi

    # A stop during the steps above (e.g. the --kerberos sleep) must not go on
    # to start the client after cleanup has run - it would be left orphaned.
    [[ "$stopping" == "1" ]] && return 0
    say "Starting KrakenSDR DoA client (headless; log: ${klog#$SCRIPT_DIR/})"
    ( cd "$KRAKEN_DIR" && KRAKEN_DOA_NO_TUI=1 KRAKEN_RESTART_EXIT="$RESTART_EXIT" exec ./kraken_doa $WB_FLAG ) >>"$klog" 2>&1 &
    kpid=$!
    echo "     Web UI: https://localhost:8080   (DoA page: http://localhost:8081)"
    echo
    say "${GRN}Stack is up.${RST} Logs are errors-only (HEIMDALL_VERBOSE_LOG=1 /"
    say "KRAKEN_DOA_VERBOSE_LOG=1 for full output). Press ${BOLD}Ctrl+C${RST} to stop everything."
    echo "-------------------------------------------------------------------------------"
    if [[ "$(uname -s)" == Darwin ]]; then
        # BSD tail has no --pid: follow the logs until heimdall ends
        # (disowned: its end is no "Terminated" job notice)
        tail -n 0 -F "$hlog" "$klog" &
        tpid=$!
        disown "$tpid" 2>/dev/null
        while kill -0 "$hpid" 2>/dev/null; do sleep 1; done
        kill "$tpid" 2>/dev/null
        tpid=""
    else
        tail -n 0 -F --pid="$hpid" "$hlog" "$klog" &
        wait "$!" 2>/dev/null || true
    fi
    # Heimdall ended. Pass its status on (it exits 0 after a clean signal
    # shutdown, 1 on a startup failure, 128+N when killed by signal N): this
    # used to return 0 even after a crash, so a container restart policy or
    # a CI check could not tell. A stop WE requested (Ctrl+C / TERM -> cleanup,
    # which already reaped it) counts as success.
    local hrc=0
    wait "$hpid" 2>/dev/null || hrc=$?
    if [[ "$stopping" == "1" || "$hrc" -eq 0 ]]; then
        warn "Heimdall process ended; shutting down."
        return 0
    fi
    # ↻ Restart software from the web page: heimdall exits with RESTART_EXIT
    # (kraken_doa only asks after heimdall accepted, and exits with it too) -
    # the client is stopped (if it isn't gone already) and the caller starts
    # the stack again
    if [[ "$hrc" == "$RESTART_EXIT" ]]; then
        say "Restart requested from the web page - restarting the stack"
        cleanup
        return "$RESTART_EXIT"
    fi
    warn "Heimdall exited with status $hrc; shutting down. Last log lines:"
    tail -n 15 "$hlog" | sed 's/^/     /'
    return "$hrc"
}

if [[ "$HEADLESS" == "1" ]]; then
    [[ "${NO_TMUX:-0}" == "1" ]] || warn "tmux not found - falling back to headless mode (no dashboards)."
    # headless: build in the foreground, then start as usual
    if [[ "$NEED_BUILD" == "1" ]]; then
        build_and_report || exit 1
    fi
    while :; do
        run_headless
        rc=$?
        [[ "$rc" == "$RESTART_EXIT" ]] || exit "$rc"
        HEADLESS_RESTARTED=1
        sleep 1
    done
fi

# ===========================================================================
# Split-screen mode (default): tmux with both native dashboards.
# ===========================================================================
if [[ "$NEED_BUILD" == "1" ]]; then
    # The build runs in the session's first window; it opens the stack's
    # window when done (__build_pane). The settings it needs go along: the
    # pane's environment is the tmux server's, not this shell's
    say "Building in tmux session '$SESSION' (window 'build'); the stack starts when it is done"
    pane_env=""
    for v in WAIT_TIMEOUT WIDEBAND KERBEROS KERBEROS_SW EXT_NOISE JOBS RESTART_DELAY \
             BUILD_PAGE_HTTP_PORT BUILD_PAGE_HTTPS_PORT HEIMDALL_VERBOSE_LOG KRAKEN_DOA_VERBOSE_LOG; do
        if [[ -n "${!v+x}" ]]; then pane_env="$pane_env $v=$(printf '%q' "${!v}")"; fi
    done
    tmux new-session -d -s "$SESSION" -n build -c "$SCRIPT_DIR" \
        "exec env TMUX_SESSION=$(printf '%q' "$SESSION")$pane_env '$SELF' __build_pane"
    # it closes when the stack's window is open (a failed build keeps it, see __build_pane)
    tmux set-option -w -t "$SESSION:build" remain-on-exit off 2>/dev/null
    tmux set-option -t "$SESSION" mouse on
    echo "     Progress: http://localhost:${BUILD_PAGE_HTTP_PORT}  https://localhost:${BUILD_PAGE_HTTPS_PORT}   log: ${BUILD_LOG#$SCRIPT_DIR/}"
else
    say "Launching split-screen stack (session '$SESSION')"
    launch_tmux_stack
fi

cat <<EOF

  ${BOLD}Split-screen is starting.${RST}$([[ "$NEED_BUILD" == "1" ]] && printf '\n    First  : the build (window "build", closes when done)')
    Top    : Heimdall server dashboard
    Bottom : convergence wait -> KrakenSDR DoA client dashboard

  Heimdall  http://localhost:8070      Client  https://localhost:8080  (DoA :8081)

  tmux keys:  Ctrl-b arrows = switch panes   Ctrl-b z = zoom pane   Ctrl-b d = detach
  Stop everything:  ${BOLD}${SELF} stop${RST}   (or Ctrl-b d then re-run;  detach leaves it running)

EOF
sleep 1
# No tty (systemd, cron, ssh -T) or NO_ATTACH=1: leave the session detached.
# The apps are already running; there is simply no client to attach.
if [[ "${NO_ATTACH:-0}" == "1" || ! -t 0 ]]; then
    ok "Session '$SESSION' running detached. Attach with: tmux attach -t $SESSION"
    exit 0
fi
# Attach - or switch, if we were launched from inside another tmux session
# (a plain 'attach' there errors with "sessions should be nested with care").
if [[ -n "${TMUX:-}" ]]; then
    warn "Already inside tmux - switching this client to '$SESSION'."
    exec tmux switch-client -t "$SESSION"
else
    exec tmux attach -t "$SESSION"
fi
