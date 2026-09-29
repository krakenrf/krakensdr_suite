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
#   WAIT_TIMEOUT=600 ./run.sh   # allow up to 600s for convergence (default 300)
#   WAIT_TIMEOUT=0 ./run.sh     # wait forever for convergence
#   NO_TMUX=1 ./run.sh          # headless: no dashboards, log to files instead
#
set -uo pipefail

# Resolve the directory this script lives in so it works from anywhere.
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname "$SELF")"
HEIMDALL_DIR="$SCRIPT_DIR/heimdall_v2"
KRAKEN_DIR="$SCRIPT_DIR/kraken_doa_v2"
LOG_DIR="$SCRIPT_DIR/logs"

WAIT_TIMEOUT="${WAIT_TIMEOUT:-300}"     # seconds to wait for convergence; 0 = forever
SESSION="${TMUX_SESSION:-krakensdr}"    # tmux session name
DATA_PORT=8091                          # Heimdall TCP data port (carries phase_state)

# Hardware-variant flags (combinable, order-independent):
#   --wideband / -w  KrakenSDR Wideband variant (passed to BOTH apps)
#   --kerberos       KerberosSDR, manual calibration (heimdall only; the
#                    client auto-detects the mode from the data stream)
#   --kerberos_sw    KerberosSDR with CKOVAL antenna switches (heimdall only)
# Matching env vars (WIDEBAND=1 / KERBEROS=1 / KERBEROS_SW=1) also work and
# are how the flags survive into the tmux client pane re-invocation.
WIDEBAND="${WIDEBAND:-0}"
KERBEROS="${KERBEROS:-0}"
KERBEROS_SW="${KERBEROS_SW:-0}"
while [[ "${1:-}" == "--wideband" || "${1:-}" == "-w" ||
         "${1:-}" == "--kerberos" || "${1:-}" == "--kerberos_sw" ]]; do
    case "$1" in
        --wideband|-w)  WIDEBAND=1 ;;
        --kerberos)     KERBEROS=1 ;;
        --kerberos_sw)  KERBEROS_SW=1 ;;
    esac
    shift
done
WB_FLAG=""
[[ "$WIDEBAND" == "1" ]] && WB_FLAG="--wideband"
# --kerberos_sw implies (and supersedes) --kerberos on the heimdall side.
KB_FLAG=""
[[ "$KERBEROS" == "1" ]] && KB_FLAG="--kerberos"
[[ "$KERBEROS_SW" == "1" ]] && KB_FLAG="--kerberos_sw"
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

# ===========================================================================
# Convergence probe: poll Heimdall's data port until phase_state == CONVERGED.
# The wire header (see heimdall_v2/src/net/tcp_data_server.cpp) is big-endian:
#   magic(4) 'MCHQ' | num_channels(4) | num_samples(4) | phase_state(4) | ...
# CONVERGED == 4 (PhaseCompensatorState enum). Each fresh connection starts on a
# packet boundary, so reading the first 16 bytes yields a valid header.
# Exit 0 = converged, 1 = timed out.  arg1 = timeout seconds (0 = forever).
# ===========================================================================
wait_for_convergence() {
    python3 - "$1" "$DATA_PORT" <<'PY'
import socket, struct, sys, time
timeout = float(sys.argv[1]); port = int(sys.argv[2])
MAGIC = 0x4D434851; CONVERGED = 4
NAMES = {0:"WAIT-LAG",1:"MEASURING",2:"APPLYING",3:"VERIFYING",
         4:"CONVERGED",5:"COOLDOWN",6:"PER-BIN"}
start = time.time()
while True:
    el = int(time.time() - start)
    if timeout > 0 and (time.time() - start) > timeout:
        sys.stderr.write("\n"); sys.exit(1)
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
            phase &= 0xFF  # high bits carry KerberosSDR flags; low byte is the state
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
        "$@" <&0 &
        child=$!
        # wait returns early (status > 128) when a trap fires; keep waiting
        # until the app itself has exited
        while :; do
            wait "$child"; rc=$?
            kill -0 "$child" 2>/dev/null || break
        done
        child=
        [[ "$stopping" == "1" || "$rc" == "0" ]] && return "$rc"
        warn "$name exited (status $rc) - restarting in ${RESTART_DELAY:-5}s (Ctrl+C to stay stopped)"
        sleep "${RESTART_DELAY:-5}"
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
        warn "then press 'Force Recalibration Now' in the heimdall web UI (:8070)."
        sleep 2
    else
        echo "${BOLD}Waiting for Heimdall phase convergence"\
"$([[ "$WAIT_TIMEOUT" -eq 0 ]] && echo " (no timeout)" || echo " (timeout ${WAIT_TIMEOUT}s)")...${RST}"
        if wait_for_convergence "$WAIT_TIMEOUT"; then
            ok "Phase converged - DF output is valid. Starting client..."
            sleep 1
        else
            warn "No convergence within ${WAIT_TIMEOUT}s. Starting client anyway"
            warn "(DoA output stays invalid until Heimdall converges - watch the top pane)."
            sleep 3
        fi
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
stop_session() {
    local pids pid alive
    pids=$(tmux list-panes -s -t "$SESSION" -F '#{pane_pid}' 2>/dev/null)
    for pid in $pids; do
        kill -TERM -- "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
    done
    for _ in $(seq 1 50); do            # up to 10 s
        alive=0
        for pid in $pids; do kill -0 "$pid" 2>/dev/null && alive=1; done
        [[ "$alive" == "0" ]] && break
        sleep 0.2
    done
    [[ "$alive" == "1" ]] && warn "Apps still running after 10s - forcing the session closed."
    tmux kill-session -t "$SESSION" 2>/dev/null || true
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
              pgrep -u "$uid" -f -- "$SELF __(supervise|client_pane)"; } 2>/dev/null | sort -un)
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
    ps -o pid=,etime=,args= -g "${pgids// /,}" | sed 's/^ */     /'
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
# `stop` subcommand: stop both apps cleanly and tear down the tmux session.
# ===========================================================================
if [[ "${1:-}" == "stop" ]]; then
    if command -v tmux >/dev/null 2>&1 && tmux has-session -t "$SESSION" 2>/dev/null; then
        stop_session
        ok "Stopped tmux session '$SESSION'."
    else
        warn "No running '$SESSION' session found."
    fi
    stop_stale
    exit 0
fi

# --- sanity checks ----------------------------------------------------------
[[ -x "$HEIMDALL_DIR/heimdall" ]] || die "heimdall not built. Run ${BOLD}${SCRIPT_DIR}/install.sh${RST} first."
[[ -x "$KRAKEN_DIR/kraken_doa" ]] || die "kraken_doa not built. Run ${BOLD}${SCRIPT_DIR}/install.sh${RST} first."
command -v python3 >/dev/null 2>&1 || die "python3 is required for convergence detection."

# Clear out a previous stack before starting a new one: a live session is
# stopped cleanly, then anything orphaned outside it (see stop_stale).
if command -v tmux >/dev/null 2>&1 && tmux has-session -t "$SESSION" 2>/dev/null; then
    warn "A '$SESSION' session is already running - replacing it."
    stop_session
fi
stop_stale

# ===========================================================================
# Headless fallback (NO_TMUX=1 or tmux missing): no dashboards; log to files.
# ===========================================================================
run_headless() {
    local hlog="$LOG_DIR/heimdall.log" klog="$LOG_DIR/kraken_doa.log"
    # NOTE: hpid/kpid are intentionally NOT 'local' - the EXIT-trap cleanup runs
    # after this function returns (e.g. when Heimdall dies) and must still see
    # them to kill the client. As locals they would be out of scope by then.
    hpid=""; kpid=""
    mkdir -p "$LOG_DIR"
    cleanup() {
        trap - INT TERM EXIT; echo
        say "Shutting down..."
        [[ -n "$kpid" ]] && kill "$kpid" 2>/dev/null || true
        [[ -n "$hpid" ]] && kill "$hpid" 2>/dev/null || true
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
    ( cd "$HEIMDALL_DIR" && HEIMDALL_NO_TUI=1 exec ./heimdall $WB_FLAG $KB_FLAG ) >"$hlog" 2>&1 &
    hpid=$!
    echo "     Web UI: http://localhost:8070"

    if [[ "$KB_MANUAL" == "1" ]]; then
        warn "KerberosSDR manual-calibration mode: skipping the convergence wait."
        warn "DoA output is INVALID until you calibrate: disconnect all antennas,"
        warn "then press 'Force Recalibration Now' in the heimdall web UI (:8070)."
        sleep 2
    else
        say "Waiting for phase convergence$([[ "$WAIT_TIMEOUT" -eq 0 ]] && echo " (no timeout)" || echo " (timeout ${WAIT_TIMEOUT}s)")..."
        if ! wait_for_convergence "$WAIT_TIMEOUT"; then
            warn "No convergence in time. Last Heimdall log lines:"
            tail -n 15 "$hlog" | sed 's/^/     /'
            die "Timed out (check antennas/noise source, or raise WAIT_TIMEOUT)."
        fi
        ok "Phase converged - DF output is valid."
    fi

    say "Starting KrakenSDR DoA client (headless; log: ${klog#$SCRIPT_DIR/})"
    ( cd "$KRAKEN_DIR" && KRAKEN_DOA_NO_TUI=1 exec ./kraken_doa $WB_FLAG ) >"$klog" 2>&1 &
    kpid=$!
    echo "     Web UI: https://localhost:8080   (DoA page: http://localhost:8081)"
    echo
    say "${GRN}Stack is up.${RST} Logs are errors-only (HEIMDALL_VERBOSE_LOG=1 /"
    say "KRAKEN_DOA_VERBOSE_LOG=1 for full output). Press ${BOLD}Ctrl+C${RST} to stop everything."
    echo "-------------------------------------------------------------------------------"
    tail -n 0 -F --pid="$hpid" "$hlog" "$klog" &
    wait "$!" 2>/dev/null || true
    warn "Heimdall process ended; shutting down."
}

if [[ "${NO_TMUX:-0}" == "1" ]] || ! command -v tmux >/dev/null 2>&1; then
    [[ "${NO_TMUX:-0}" == "1" ]] || warn "tmux not found - falling back to headless mode (no dashboards)."
    run_headless
    exit 0
fi

# ===========================================================================
# Split-screen mode (default): tmux with both native dashboards.
# ===========================================================================
say "Launching split-screen stack (session '$SESSION')"

# Top pane: Heimdall with its live dashboard (real pty -> TUI renders).
# Supervised: restarted if it crashes (see supervise).
tmux new-session -d -s "$SESSION" -n krakensdr -c "$HEIMDALL_DIR" \
    "exec '$SELF' __supervise Heimdall ./heimdall $WB_FLAG $KB_FLAG"

# Keep dead panes visible so a crash leaves its message on screen for diagnosis.
# (remain-on-exit is a WINDOW option -> set with -w; do it before anything can exit.)
tmux set-option -w -t "$SESSION:krakensdr" remain-on-exit on
tmux set-option -t "$SESSION" mouse on

# Bottom pane: wait-for-convergence, then the client. Re-invokes this script's
# internal entrypoint; WAIT_TIMEOUT and the variant flags are passed through
# explicitly as env vars.
tmux split-window -v -t "$SESSION:krakensdr" -c "$KRAKEN_DIR" \
    "exec env WAIT_TIMEOUT='$WAIT_TIMEOUT' WIDEBAND='$WIDEBAND' KERBEROS='$KERBEROS' KERBEROS_SW='$KERBEROS_SW' '$SELF' __client_pane"

tmux select-pane -t "$SESSION:krakensdr.0"

cat <<EOF

  ${BOLD}Split-screen is starting.${RST}
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
