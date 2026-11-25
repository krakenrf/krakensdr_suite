#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/librtlsdr-update.sh - bring a librtlsdr clone that lacks the newest
# fork feature heimdall uses up to the fork's latest, SOFTLY: heimdall builds
# and runs without it (its fallbacks), so whatever stands in the way - no
# network, no upstream, a detached HEAD, local commits or changes, not a git
# clone, no git - leaves the clone as it is, with one line saying what that
# costs and how to get it. Nothing local is ever discarded, stashed or reset:
# the only change is a fast-forward merge, which git refuses when it would
# overwrite a local change. Used by heimdall_v2/Makefile (`update`, while
# make reads its rules, so the build that follows sees the new sources) and
# scripts/autobuild.sh (`due`: run.sh's "is a build needed" check). The
# marks live in heimdall_v2/Makefile (RTLSDR_FIX_MARK, RTLSDR_FEATURE_MARK);
# docs/dev/librtlsdr-fork.md, *Keeping the clone current*.
#
#   librtlsdr-update.sh due    DIR FIX_MARK FEATURE_MARK
#       exit 0 = an attempt is due. Read-only, no network: DIR's librtlsdr.c
#       has FIX_MARK (an older clone is the Makefile's hard path), its
#       include/rtl-sdr.h lacks FEATURE_MARK, and the stamp of a failed
#       attempt (DIR/build/.fork_feature_tried: HEAD commit + branch) is
#       missing, names another commit or branch, or is 7 days old or more -
#       a Pi that first started offline gets the update a week later, a
#       clone put right by hand at once (new HEAD / branch) or a week later;
#       never on every start.
#   librtlsdr-update.sh update DIR FIX_MARK FEATURE_MARK
#       if due: git fetch (bounded, below) + merge --ff-only @{u}; else
#       nothing. Writes the stamp after a failed attempt. Always exit 0.
#   librtlsdr-update.sh fetch DIR
#       `git fetch -q` in DIR, bounded; its exit status (> 128: cut off).
#
# The bound: no credential or host-key prompt (GIT_TERMINAL_PROMPT=0, stdin
# /dev/null, ssh BatchMode + ConnectTimeout unless GIT_SSH_COMMAND is set),
# an HTTP transfer below 1 KB/s for 10 s is dropped (http.lowSpeed*), and the
# whole fetch is killed after RTLSDR_FETCH_SECS (default 30) - git has no
# connect timeout of its own, and a black-holed address otherwise waits out
# the kernel's SYN retries (~2 min on Linux). Plain sh: Linux, macOS (no
# `timeout` binary there), MSYS2. Messages go to stderr: make's $(shell)
# keeps stdout.

FETCH_SECS="${RTLSDR_FETCH_SECS:-30}"
RETRY_DAYS=7

say() { echo "$*" >&2; }

# the clone's HEAD commit and branch; "- -" without them (no git, or not a
# clone of its own - git -C would find the suite's repository above it)
head_of() {
    if [ -e "$1/.git" ] && command -v git >/dev/null 2>&1; then
        echo "$(git -C "$1" rev-parse -q --verify HEAD 2>/dev/null || echo -)" \
             "$(git -C "$1" symbolic-ref -q --short HEAD 2>/dev/null || echo -)"
    else
        echo "- -"
    fi
}

stamp_file() { echo "$1/build/.fork_feature_tried"; }

due() {   # DIR FIX FEATURE
    [ -f "$1/src/librtlsdr.c" ] && [ -f "$1/include/rtl-sdr.h" ] || return 1
    grep -q "$2" "$1/src/librtlsdr.c" 2>/dev/null || return 1
    grep -q "$3" "$1/include/rtl-sdr.h" 2>/dev/null && return 1
    st="$(stamp_file "$1")"
    [ -f "$st" ] || return 0
    [ "$(cat "$st" 2>/dev/null)" = "$(head_of "$1")" ] || return 0
    [ -n "$(find "$st" -mtime +$((RETRY_DAYS - 1)) 2>/dev/null)" ]
}

# a process and its children (git fetch -> git remote-https, which would
# otherwise sit in connect() until the kernel gives up); without pgrep the
# children end by themselves when that connect fails
kill_tree() {
    for c in $(pgrep -P "$1" 2>/dev/null); do kill_tree "$c"; done
    kill "$1" 2>/dev/null
}

fetch() {   # DIR
    log="$1/build/.fork_fetch.log"
    mkdir -p "$1/build" 2>/dev/null || log=/dev/null
    GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh -o BatchMode=yes -o ConnectTimeout=10}" \
        git -C "$1" -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=10 fetch -q \
        < /dev/null > "$log" 2>&1 &
    pid=$!
    # the watchdog's output away from make's $(shell) pipe: its sleep may
    # outlive this script by up to FETCH_SECS and must not hold make up
    ( sleep "$FETCH_SECS"; kill_tree "$pid" ) < /dev/null > /dev/null 2>&1 &
    dog=$!
    wait "$pid"
    rc=$?
    kill "$dog" 2>/dev/null
    return $rc
}

# a failed attempt (or one that left the mark missing): not again before
# the HEAD or branch changes or RETRY_DAYS pass
stamp() {   # DIR
    mkdir -p "$1/build" 2>/dev/null && head_of "$1" > "$(stamp_file "$1")"
}

# the one line: what the build goes on without, and how to get it
not_updated() {   # DIR reason [hint]
    say "librtlsdr: NOT updated to the fork's latest ($2) - heimdall builds with the clone as it is, without the newest fork features it can use (sweep hops and region gains in fewer USB transfers, ~2 s instead of 25-30 s back to coherent mode after sweep / independent mode); to get them: ${3:+$3, then }git -C librtlsdr pull --ff-only, or ./install.sh"
    stamp "$1"
}

update() {   # DIR FIX FEATURE
    d="$1"
    due "$@" || return 0
    if ! command -v git >/dev/null 2>&1; then not_updated "$d" "git not found"; return 0; fi
    if [ ! -e "$d/.git" ]; then
        not_updated "$d" "not a git clone" "replace it with a clone of https://github.com/krakenrf/librtlsdr"; return 0
    fi
    old="$(git -C "$d" rev-parse -q --verify HEAD 2>/dev/null)"
    if ! git -C "$d" symbolic-ref -q HEAD > /dev/null 2>&1; then
        not_updated "$d" "a detached HEAD - a checkout of a commit" "git -C librtlsdr switch master"; return 0
    fi
    if ! git -C "$d" rev-parse -q --verify '@{u}' > /dev/null 2>&1; then
        not_updated "$d" "its branch has no upstream" "git -C librtlsdr branch -u origin/master"; return 0
    fi
    say "librtlsdr: the clone lacks $3 - fetching the fork's latest (at most ${FETCH_SECS} s)"
    fetch "$d"
    rc=$?
    if [ "$rc" -gt 128 ]; then
        not_updated "$d" "the fetch took longer than ${FETCH_SECS} s"; return 0
    elif [ "$rc" -ne 0 ]; then
        why="$(grep -m 1 -E '^(fatal|error):' "$d/build/.fork_fetch.log" 2>/dev/null | tr -d '\r')"
        not_updated "$d" "git fetch failed, offline?${why:+ $why}"; return 0
    fi
    u="$(git -C "$d" rev-parse --abbrev-ref '@{u}' 2>/dev/null)"
    if git -C "$d" merge-base --is-ancestor '@{u}' HEAD 2>/dev/null; then
        not_updated "$d" "$u has nothing newer: it lacks $3 too"; return 0
    fi
    if ! git -C "$d" merge-base --is-ancestor HEAD '@{u}' 2>/dev/null; then
        not_updated "$d" "it has local commits $u doesn't" "rebase them onto $u"; return 0
    fi
    if ! git -c core.autocrlf=false -C "$d" merge -q --ff-only '@{u}' > "$d/build/.fork_fetch.log" 2>&1; then
        not_updated "$d" "local changes there are in the way" "commit or stash them"; return 0
    fi
    say "librtlsdr: updated $(echo "$old" | cut -c1-7) -> $(git -C "$d" rev-parse --short HEAD) ($u); the library and heimdall are rebuilt"
    if grep -q "$3" "$d/include/rtl-sdr.h"; then
        rm -f "$(stamp_file "$d")"
    else
        say "librtlsdr: $u lacks $3 too - heimdall builds with what it has"
        stamp "$d"
    fi
    return 0
}

cmd="${1:-}"
[ $# -gt 0 ] && shift
case "$cmd" in
    due)    [ $# -eq 3 ] || exit 2; due "$@" ;;
    update) [ $# -eq 3 ] || exit 2; update "$@"; exit 0 ;;
    fetch)  [ $# -eq 1 ] || exit 2; fetch "$1" ;;
    *) sed -n '/^#   librtlsdr-update.sh due/,/^# The bound/p' "$0" >&2; exit 2 ;;
esac
