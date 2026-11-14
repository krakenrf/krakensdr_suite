#!/usr/bin/env bash
#
# scripts/autobuild.sh - is heimdall / kraken_doa built for THIS machine, and
# build them if not. run.sh runs `check` on every start and `build` when it
# says a build is needed (with scripts/build_page.py showing the progress on
# the web UI ports); it also works on its own.
#
# Usage:
#   scripts/autobuild.sh check [heimdall|kraken_doa]
#       exit 0 = built for this machine and up to date, 1 = a build is needed
#       (what and why on stdout). Read-only, no network, no make: compares
#       the flags stamps' "| cpu <id>" with this CPU and file times (sources
#       newer than the executable).
#   scripts/autobuild.sh runnable [heimdall|kraken_doa]
#       exit 0 = the executable exists and was not built for another CPU
#       (it may be out of date) - run.sh's "start the old build" test.
#   scripts/autobuild.sh build --status FILE --log FILE [--app heimdall|kraken_doa]
#       build what `check` says is needed (`make -j$JOBS` in the app folder,
#       as install.sh does), output to the terminal and FILE (--log), the
#       progress JSON rewritten about twice a second (--status; the contract
#       is in docs/dev/run-and-deploy.md). Exit 0 = built. Never runs sudo,
#       apt or brew: missing dependencies fail the build with a hint to run
#       ./install.sh once.
#   JOBS=2 scripts/autobuild.sh build ...   parallel jobs (default: by RAM,
#                                           1-3, like install.sh)
#
# Bash 3.2 compatible (macOS).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
H="$ROOT/heimdall_v2"
K="$ROOT/kraken_doa_v2"
R="$ROOT/librtlsdr"
# set by a successful `build` (its start time): sources older than this are built
OK_STAMP_H="$H/build/.autobuild_ok"
OK_STAMP_K="$K/obj/.autobuild_ok"

usage() { sed -n '/^# Usage:/,/^# Bash 3.2/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; }

# The CPU the Makefiles' flags stamps name ("| cpu <id>"): the same command
# as HOST_CPU in heimdall_v2/Makefile, kraken_doa_v2/Makefile,
# plugins/Makefile and LIQUID_HOST_CPU in liquid.mk - keep them alike
HOST_CPU="$(sed -n -E 's/^(CPU part|model name)[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null |
            LC_ALL=C sort -u | LC_ALL=C tr -c 'A-Za-z0-9._\n-' '_' | paste -sd+ - 2>/dev/null)"
ARCH="$(uname -m)"

REASONS=""
need() { REASONS="${REASONS}${REASONS:+; }$*"; }
rel() { echo "${1#$ROOT/}"; }

# first line of a file (no subprocess)
first_line() { local l=""; IFS= read -r l < "$1" 2>/dev/null; printf '%s' "$l"; }

# A Makefile flags stamp (FLAGS_TEXT): missing = make rebuilds everything; a
# "| cpu X" part must name this CPU; "native" without one = made before the
# stamp named the CPU (make rebuilds it: its FLAGS_TEXT differs).
# $2 = strict: 1 for check, 0 for runnable (an old stamp without a CPU is
# accepted there - most likely built on this machine)
stamp_ok() {
    local f="$1" strict="$2" text cpu
    if [[ ! -f "$f" ]]; then
        need "no flags stamp $(rel "$f") (never built by its Makefile)"
        return 1
    fi
    text="$(first_line "$f")"
    if [[ "$text" == *" | cpu "* ]]; then
        cpu="${text##* | cpu }"
        if [[ "$cpu" != "$HOST_CPU" ]]; then
            need "$(rel "$f") says built for CPU '$cpu', this machine is '${HOST_CPU:-unknown}'"
            return 1
        fi
    elif [[ "$text" == *native* && -n "$HOST_CPU" ]]; then
        [[ "$strict" == 1 ]] && need "$(rel "$f") names no CPU (built before the stamp did, maybe elsewhere)" && return 1
    fi
    return 0
}

# liquid-dsp's stamp (LIQUID_STAMP in liquid.mk): "<commit>[ cpu <id>]"
liquid_ok() {
    local strict="$1" a="$K/liquid-dsp/build-kraken/libliquid.a" f="$K/liquid-dsp/build-kraken/commit"
    local want text cpu
    if [[ ! -f "$a" || ! -f "$f" ]]; then
        # (kraken_doa links it statically: no build left = nothing to tell)
        [[ "$strict" == 1 ]] || return 0
        need "liquid-dsp not built ($(rel "$a"))"
        return 1
    fi
    text="$(first_line "$f")"
    want="$(sed -n 's/^LIQUID_COMMIT[[:space:]]*?=[[:space:]]*//p' "$K/liquid.mk" 2>/dev/null)"
    if [[ "$strict" == 1 && -n "$want" && "${text%% *}" != "$want" ]]; then
        need "liquid-dsp was built from ${text%% *}, liquid.mk pins $want"
        return 1
    fi
    if [[ "$text" == *" cpu "* ]]; then
        cpu="${text##* cpu }"
        if [[ "$cpu" != "$HOST_CPU" ]]; then
            need "liquid-dsp was built for CPU '$cpu', this machine is '${HOST_CPU:-unknown}'"
            return 1
        fi
    elif [[ "$strict" == 1 && -n "$HOST_CPU" ]]; then
        case "$ARCH" in x86_64|aarch64|armv7l)
            need "liquid-dsp's stamp names no CPU (built before it did, maybe elsewhere)"; return 1 ;;
        esac
    fi
    return 0
}

# the newer of the executable and the last successful autobuild's start
ref_file() {
    if [[ -f "$2" && "$2" -nt "$1" ]]; then echo "$2"; else echo "$1"; fi
}

# a source file newer than $1 among the rest (dirs / files); prints the first
newer_source() {
    local ref="$1"; shift
    local paths="" p
    for p in "$@"; do [[ -e "$p" ]] && paths="$paths
$p"; done
    [[ -z "$paths" ]] && return 1
    local IFS=$'\n'
    # shellcheck disable=SC2086
    find $paths -name build -prune -o -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \
        -o -name Makefile -o -name '*.mk' -o -name CXXFLAGS -o -name LICENSE \) -newer "$ref" -print 2>/dev/null |
        head -n 1
}

# uWebSockets checkout at the pinned commit with uSockets built ($1 = app dir)
uws_ok() {
    local d="$1/uWebSockets" want head
    if [[ ! -d "$d" ]]; then need "uWebSockets not fetched ($(rel "$d"))"; return 1; fi
    want="$(sed -n 's/^UWS_COMMIT[[:space:]]*?=[[:space:]]*//p' "$1/Makefile" 2>/dev/null)"
    if [[ -f "$d/.git/HEAD" && -n "$want" ]]; then
        head="$(first_line "$d/.git/HEAD")"
        [[ "$head" == "$want" ]] || { need "uWebSockets is not at the pinned $want"; return 1; }
    fi
    if ! compgen -G "$d/uSockets/*.o" >/dev/null; then need "uSockets not built"; return 1; fi
    return 0
}

check_heimdall() {   # sets REASONS; 0 = up to date
    REASONS=""
    local exe="$H/heimdall" ref f
    if [[ ! -x "$exe" ]]; then need "no executable $(rel "$exe")"; return 1; fi
    stamp_ok "$H/build/.flags" 1 || return 1
    ref="$(ref_file "$exe" "$OK_STAMP_H")"
    f="$(newer_source "$ref" "$H/src" "$H/external" "$H/config.h" "$H/Makefile")"
    [[ -n "$f" ]] && { need "$(rel "$f") is newer than the build"; return 1; }
    uws_ok "$H" || return 1
    local lib="$R/build/src/librtlsdr.a"
    if [[ ! -f "$lib" ]]; then need "the librtlsdr fork's static library is not built ($(rel "$lib"))"; return 1; fi
    grep -qs rtlsdr_finish_tune "$R/src/librtlsdr.c" ||
        { need "librtlsdr/ predates the fork heimdall needs (578b43d)"; return 1; }
    f="$(newer_source "$lib" "$R/src" "$R/include")"
    [[ -n "$f" ]] && { need "$(rel "$f") is newer than the librtlsdr build"; return 1; }
    return 0
}

check_kraken() {
    REASONS=""
    local exe="$K/kraken_doa" ref f
    if [[ ! -x "$exe" ]]; then need "no executable $(rel "$exe")"; return 1; fi
    stamp_ok "$K/obj/.flags" 1 || return 1
    liquid_ok 1 || return 1
    if [[ ! -f "$K/plugins/lib/build/libkrakendig.a" ]]; then need "decoder plugins not built"; return 1; fi
    stamp_ok "$K/plugins/lib/build/.flags" 1 || return 1
    ref="$(ref_file "$exe" "$OK_STAMP_K")"
    f="$(newer_source "$ref" "$K/src" "$K/include" "$K/external" "$K/Makefile" "$K/liquid.mk" "$K/plugins")"
    [[ -n "$f" ]] && { need "$(rel "$f") is newer than the build"; return 1; }
    uws_ok "$K" || return 1
    return 0
}

runnable_heimdall() {
    REASONS=""
    [[ -x "$H/heimdall" ]] || { need "no executable"; return 1; }
    stamp_ok "$H/build/.flags" 0
}
runnable_kraken() {
    REASONS=""
    [[ -x "$K/kraken_doa" ]] || { need "no executable"; return 1; }
    stamp_ok "$K/obj/.flags" 0 && liquid_ok 0
}

# check/runnable over the apps named in $2 (default both); prints a line each
run_checks() {   # <check|runnable> [app]
    local what="$1" app="${2:-}" rc=0 a
    for a in heimdall kraken_doa; do
        [[ -n "$app" && "$app" != "$a" ]] && continue
        if "${what}_${a/_doa/}"; then
            echo "$a: ${what/check/up to date}"
        else
            [[ "$what" == check ]] && echo "$a: needs a build - $REASONS" || echo "$a: not runnable - $REASONS"
            rc=1
        fi
    done
    return "$rc"
}

cmd="${1:-}"
[[ $# -gt 0 ]] && shift
case "$cmd" in
    check|runnable)
        case "${1:-}" in ""|heimdall|kraken_doa) ;; *) usage >&2; exit 2 ;; esac
        run_checks "$cmd" "${1:-}"
        exit $?
        ;;
    build) ;;
    -h|--help|help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

# ---------------------------------------------------------------- build ----
STATUS="" LOG="" ONLY=""
while (( $# )); do
    case "$1" in
        --status) STATUS="${2:-}"; shift ;;
        --log)    LOG="${2:-}"; shift ;;
        --app)    ONLY="${2:-}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "autobuild.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
[[ -n "$STATUS" && -n "$LOG" ]] || { echo "autobuild.sh build: --status and --log are required" >&2; exit 2; }
case "$ONLY" in ""|heimdall|kraken_doa) ;; *) echo "autobuild.sh: --app heimdall|kraken_doa" >&2; exit 2 ;; esac

# Build parallelism - exactly install.sh's: each g++ job on the Eigen-heavy
# files needs ~1 GB+ at -O3; never more than 3 on a Pi
if [[ -z "${JOBS:-}" ]]; then
    if [[ "$(uname -s)" == Darwin ]]; then
        MEM_MB=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1048576 ))
    else
        MEM_MB="$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)"
    fi
    if   (( MEM_MB >= 7000 )); then JOBS=3
    elif (( MEM_MB >= 3500 )); then JOBS=2
    else                            JOBS=1
    fi
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "autobuild.sh: JOBS must be a positive number" >&2; exit 2; }

# what to build, and why
APPS="" WHY=""
if [[ -z "$ONLY" || "$ONLY" == heimdall ]] && ! check_heimdall; then
    APPS="heimdall"; WHY="heimdall: $REASONS"
fi
if [[ -z "$ONLY" || "$ONLY" == kraken_doa ]] && ! check_kraken; then
    APPS="${APPS:+$APPS }kraken_doa"; WHY="${WHY:+$WHY
}kraken_doa: $REASONS"
fi

command -v python3 >/dev/null 2>&1 || { echo "autobuild.sh: python3 is required" >&2; exit 1; }
mkdir -p "$(dirname "$STATUS")" "$(dirname "$LOG")" || exit 1

export AB_ROOT="$ROOT" AB_JOBS="$JOBS" AB_STATUS="$STATUS" AB_LOG="$LOG" AB_APPS="$APPS" AB_WHY="$WHY"
export AB_OK_H="$OK_STAMP_H" AB_OK_K="$OK_STAMP_K"
# The build itself: python runs make, parses its output for the progress and
# writes the status file (the progress model: docs/dev/run-and-deploy.md)
exec python3 - <<'PY'
import json, os, re, shutil, signal, subprocess, sys, threading, time
from collections import deque

ROOT = os.environ["AB_ROOT"]
JOBS = int(os.environ["AB_JOBS"])
STATUS = os.environ["AB_STATUS"]
LOG = os.environ["AB_LOG"]
APPS = os.environ["AB_APPS"].split()
WHY = os.environ["AB_WHY"]
OK_STAMP = {"heimdall": os.environ["AB_OK_H"], "kraken_doa": os.environ["AB_OK_K"]}
H = os.path.join(ROOT, "heimdall_v2")
K = os.path.join(ROOT, "kraken_doa_v2")
R = os.path.join(ROOT, "librtlsdr")
APP_DIR = {"heimdall": H, "kraken_doa": K}
INSTALL_HINT = "run ./install.sh once (it installs the build dependencies), then ./run.sh again"

# ---- relative cost of each kind of step ----
# 1 unit = one kraken_doa object (3.2 s of one job slot in the calibration
# build, x86_64 -j3; see docs/dev/run-and-deploy.md)
W_KOBJ = 1.0           # a kraken_doa object (src/*.cpp: uWebSockets, Eigen, liquid headers)
W_HOBJ = 0.55          # a heimdall object (1.7 s)
W_PFFFT = 0.2          # each pffft object
W_LIBOBJ = 0.6         # each plugins/lib object (and the plugin host, the codec check)
W_PLUGIN_BASE = 0.3    # a plugin = one g++ run over all its files + link:
W_PLUGIN_PER_KB = 0.021  # 1 s + 0.065 s per KB of its .cpp/.hpp (measured, 28 plugins)
W_LIQUID = 7.0         # liquid-dsp: fetch + cmake configure + 159 C files
W_CLONE = 1.5          # a git clone (network bound: worth less on a slow CPU)
W_USOCKETS = 0.5       # uSockets (a dozen small C files)
W_RTL = 2.0            # the librtlsdr fork: cmake configure + rtlsdr_static
W_LINK = 0.5
DEFAULT_SECS_PER_UNIT = 10.0   # until the first compile step has finished

def mtime(p):
    try:
        return os.stat(p).st_mtime
    except OSError:
        return None

def read(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""

def first_line(p):
    return read(p).split("\n", 1)[0].strip()

def host_cpu():
    try:
        out = subprocess.run(["sh", "-c",
            "sed -n -E 's/^(CPU part|model name)[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null"
            " | LC_ALL=C sort -u | LC_ALL=C tr -c 'A-Za-z0-9._\\n-' '_' | paste -sd+ -"],
            capture_output=True, text=True).stdout.strip()
    except OSError:
        out = ""
    return out
HOST_CPU = host_cpu()

def stamp_matches(path):
    """True when a Makefile flags stamp exists and doesn't name another CPU
    (a stamp from before the CPU part counts as a mismatch, as for make)."""
    t = first_line(path)
    if not t:
        return False
    if " | cpu " in t:
        return t.rsplit(" | cpu ", 1)[1] == HOST_CPU
    return not ("native" in t and HOST_CPU)

def sources_from_makefile(mk, srcdir_prefix):
    """The app's .cpp list from its Makefile text (SOURCES / *_SRCS), without
    the Windows-only ones"""
    txt = read(mk)
    found = []
    for m in re.finditer(r"(?:\$\(SRCDIR\)|\bsrc)/([\w/.-]+\.cpp)", txt):
        s = "src/" + m.group(1)
        if s.endswith("_win.cpp") or s in found:
            continue
        if os.path.isfile(os.path.join(os.path.dirname(mk), s)):
            found.append(s)
    return found

def deps_newer(dfile, obj_t, base):
    """an object out of date by its .d file (a dependency newer or gone)"""
    txt = read(dfile)
    if not txt:
        return True
    body = txt.replace("\\\n", " ").split("\n", 1)[0]
    if ":" not in body:
        return True
    for d in body.split(":", 1)[1].split():
        t = mtime(d if os.path.isabs(d) else os.path.join(base, d))
        if t is None or t > obj_t:
            return True
    return False

def stale_objects(app_dir, srcs, objdir, depdir, all_stale):
    out = []
    for s in srcs:
        rel = s[len("src/"):]
        obj = os.path.join(app_dir, objdir, rel[:-4] + ".o")
        if all_stale:
            out.append(s); continue
        ot = mtime(obj)
        st = mtime(os.path.join(app_dir, s))
        if ot is None or st is None or st > ot:
            out.append(s); continue
        dfile = os.path.join(app_dir, depdir, rel[:-4] + ".d")
        if deps_newer(dfile, ot, app_dir):
            out.append(s)
    return out

# ---- the plan: every step the build is expected to run, with a weight ----
class Item:
    def __init__(self, key, app, phase, weight, label, kind="obj"):
        self.key, self.app, self.phase, self.weight, self.label, self.kind = key, app, phase, weight, label, kind
        self.state = "todo"      # todo / run / done
        self.t0 = None
        self.frac = 0.0          # explicit progress (cmake's [ NN%])

items = {}
order = []
def add(key, app, phase, weight, label, kind="obj"):
    if key not in items:
        it = Item(key, app, phase, weight, label, kind)
        items[key] = it
        order.append(it)
    return items[key]

def uws_plan(app, d):
    u = os.path.join(d, "uWebSockets")
    if not os.path.isdir(u):
        add(app + ":uws-clone", app, app + ": uWebSockets", W_CLONE, "Fetching uWebSockets", "explicit")
    if not any(f.endswith(".o") for f in (os.listdir(os.path.join(u, "uSockets")) if os.path.isdir(os.path.join(u, "uSockets")) else [])):
        add(app + ":usockets", app, app + ": uWebSockets", W_USOCKETS, "Building uSockets", "explicit")

def plan_heimdall():
    app = "heimdall"
    uws_plan(app, H)
    lib = os.path.join(R, "build", "src", "librtlsdr.a")
    rtl_build = False
    if not os.path.isfile(os.path.join(R, "CMakeLists.txt")):
        add("rtl-clone", app, "heimdall: librtlsdr", W_CLONE, "Fetching the librtlsdr fork", "explicit")
        rtl_build = True
    lt = mtime(lib)
    if lt is None or rtl_build:
        rtl_build = True
    else:
        for sub in ("src", "include"):
            p = os.path.join(R, sub)
            for f in (os.listdir(p) if os.path.isdir(p) else []):
                if f.endswith((".c", ".h")) and (mtime(os.path.join(p, f)) or 0) > lt:
                    rtl_build = True
    if rtl_build:
        add("rtl", app, "heimdall: librtlsdr", W_RTL, "Building the librtlsdr fork", "explicit")
    all_stale = not stamp_matches(os.path.join(H, "build", ".flags"))
    srcs = sources_from_makefile(os.path.join(H, "Makefile"), "src")
    stale = stale_objects(H, srcs, "build", "build", all_stale)
    for s in stale:
        add("h:" + s, app, "heimdall", W_HOBJ, "Compiling " + s)
    for p in ("pffft", "pffft_common"):
        o = os.path.join(H, "build", "pffft", p + ".o")
        if all_stale or mtime(o) is None:
            add("h:external/pffft/%s.c" % p, app, "heimdall", W_PFFFT, "Compiling external/pffft/%s.c" % p)
    exe = os.path.join(H, "heimdall")
    if stale or rtl_build or mtime(exe) is None or all_stale or \
            (mtime(os.path.join(H, "config.h")) or 0) > (mtime(exe) or 0):
        add("h:link", app, "heimdall", W_LINK, "Linking heimdall", "explicit")

def plan_kraken():
    app = "kraken_doa"
    uws_plan(app, K)
    lb = os.path.join(K, "liquid-dsp", "build-kraken")
    want = ""
    m = re.search(r"^LIQUID_COMMIT\s*\?=\s*(\S+)", read(os.path.join(K, "liquid.mk")), re.M)
    if m:
        want = m.group(1)
    stamp = first_line(os.path.join(lb, "commit"))
    liquid = (mtime(os.path.join(lb, "libliquid.a")) is None or not stamp.startswith(want)
              or (" cpu " in stamp and stamp.rsplit(" cpu ", 1)[1] != HOST_CPU)
              or (" cpu " not in stamp and HOST_CPU and os.uname().machine in ("x86_64", "aarch64", "armv7l")))
    if liquid:
        add("liquid", app, "kraken_doa: liquid-dsp", W_LIQUID, "Building liquid-dsp", "explicit")
    all_stale = liquid or not stamp_matches(os.path.join(K, "obj", ".flags"))
    srcs = sources_from_makefile(os.path.join(K, "Makefile"), "src")
    stale = stale_objects(K, srcs, "obj", "dep", all_stale)
    for s in stale:
        add("k:" + s, app, "kraken_doa", W_KOBJ, "Compiling " + s)
    for p in ("pffft", "pffft_common"):
        if all_stale or mtime(os.path.join(K, "obj", "pffft", p + ".o")) is None:
            add("k:external/pffft/%s.c" % p, app, "kraken_doa", W_PFFFT, "Compiling external/pffft/%s.c" % p)
    exe = os.path.join(K, "kraken_doa")
    if stale or liquid or all_stale or mtime(exe) is None:
        add("k:link", app, "kraken_doa", W_LINK, "Linking kraken_doa", "explicit")
    plan_plugins(liquid)

def plan_plugins(liquid):
    app = "kraken_doa"
    P = os.path.join(K, "plugins")
    lbd = os.path.join(P, "lib", "build")
    all_stale = liquid or not stamp_matches(os.path.join(lbd, ".flags"))
    hdr_t = 0
    for d in ("lib", "sdk"):
        for f in os.listdir(os.path.join(P, d)) if os.path.isdir(os.path.join(P, d)) else []:
            if f.endswith(".hpp"):
                hdr_t = max(hdr_t, mtime(os.path.join(P, d, f)) or 0)
    lib_objs = [(f[:-4], os.path.join(P, "lib", f)) for f in sorted(os.listdir(os.path.join(P, "lib")))
                if f.endswith(".cpp")] if os.path.isdir(os.path.join(P, "lib")) else []
    lib_objs += [("pffft", None), ("pffft_common", None), ("kproc", None), ("fs_compat", None)]
    lib_stale = False
    for name, src in lib_objs:
        o = os.path.join(lbd, name + ".o")
        ot = mtime(o)
        if all_stale or ot is None or (src and ((mtime(src) or 0) > ot or hdr_t > ot)):
            w = W_PFFFT if name.startswith("pffft") else W_LIBOBJ
            add("p:lib/" + name, app, "decoder plugins", w, "Compiling plugins/lib " + name)
            lib_stale = True
    host = os.path.join(P, "sdk", "build", "plugin_host.o")
    if all_stale or mtime(host) is None or hdr_t > (mtime(host) or 0):
        add("p:host", app, "decoder plugins", W_LIBOBJ, "Compiling the plugin host")
        lib_stale = True
    cod = os.path.join(lbd, "codecs")
    if lib_stale or mtime(cod) is None:
        add("p:codecs", app, "decoder plugins", W_LIBOBJ, "Building the codec check")
    for name in sorted(os.listdir(P)):
        d = os.path.join(P, name)
        if not os.path.isfile(os.path.join(d, "decoder.cpp")):
            continue
        size = 0
        newest = 0
        for f in os.listdir(d):
            fp = os.path.join(d, f)
            if f.endswith((".cpp", ".hpp", ".h")) or f in ("CXXFLAGS", "LICENSE"):
                try:
                    st = os.stat(fp)
                except OSError:
                    continue
                if f.endswith((".cpp", ".hpp", ".h")):
                    size += st.st_size
                newest = max(newest, st.st_mtime)
        exe = os.path.join(d, "build", "decoder")
        et = mtime(exe)
        if lib_stale or all_stale or et is None or newest > et or hdr_t > et:
            add("p:" + name, app, "decoder plugins", W_PLUGIN_BASE + W_PLUGIN_PER_KB * size / 1024.0,
                "Building plugin " + name)

# ---- status file ----
status_lock = threading.Lock()
t_start = int(time.time())
state = {"state": "building", "percent": 0, "phase": "preparing", "step": "", "error": ""}
tail = deque(maxlen=15)
CTRL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]|\x1b\[[0-9;?]*[A-Za-z]")

def write_status():
    with status_lock:
        done_n = sum(1 for it in order if it.state == "done")
        obj = {"state": state["state"], "percent": int(state["percent"]), "phase": state["phase"],
               "step": state["step"], "done": done_n, "total": len(order), "started": t_start,
               "updated": int(time.time()), "log_tail": list(tail)}
        if state["state"] == "failed":
            obj["error"] = state["error"]
        tmp = STATUS + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(obj, f, ensure_ascii=True)
            os.replace(tmp, STATUS)
        except OSError as e:
            sys.stderr.write("autobuild: cannot write %s: %s\n" % (STATUS, e))

# seconds per weight unit per job slot, learned from finished compile steps
learn = {"secs": 0.0, "units": 0.0}

def compute_percent():
    total = sum(it.weight for it in order)
    if total <= 0:
        return state["percent"]
    now = time.time()
    rate = learn["secs"] / learn["units"] if learn["units"] > 0 else None
    done = 0.0
    for it in order:
        if it.state == "done":
            done += it.weight
        elif it.state == "run":
            if it.frac > 0:
                credit = it.frac
            else:
                # by time: the learned rate, or a guess before the first step ends
                credit = min(0.85, (now - it.t0) / ((rate or DEFAULT_SECS_PER_UNIT) * it.weight))
            done += it.weight * min(0.95, credit)
    p = int(100.0 * done / total)
    return max(state["percent"], min(99, p))

def ticker():
    while not stop_ticker.is_set():
        with status_lock:
            try:
                poll_liquid()   # liquid's cmake writes to its own log, not to make's output
                state["percent"] = compute_percent()
            except Exception as e:
                sys.stderr.write("autobuild: progress: %r\n" % e)
        write_status()
        stop_ticker.wait(0.5)
stop_ticker = threading.Event()

# ---- output parsing ----
running_fifo = []   # compile steps in flight, oldest first (parallel make: JOBS slots)

def start(it, label=None):
    if it.state != "todo":
        return
    it.state = "run"
    it.t0 = time.time()
    state["phase"] = it.phase
    state["step"] = label or it.label
    if it.kind == "obj":
        running_fifo.append(it)
        # a new compile started: with every slot busy, the oldest one ended
        while len(running_fifo) > JOBS:
            finish(running_fifo[0])

def finish(it):
    if it.state == "done":
        return
    if it.state == "run" and it.kind == "obj" and it.t0:
        learn["secs"] += time.time() - it.t0
        learn["units"] += it.weight
    it.state = "done"
    if it in running_fifo:
        running_fifo.remove(it)

def finish_where(pred):
    for it in order:
        if pred(it):
            finish(it)

def unplanned(key, app, phase, weight, label, kind="obj"):
    it = items.get(key)
    if it is None:
        it = add(key, app, phase, weight, label, kind)
    return it

RE_COMPILING = re.compile(r"^Compiling ((?:src|external)/\S+?)(?:\.\.\.)?$")
RE_PCT = re.compile(r"^\[\s*(\d+)%\]")
RE_PLUG_LIB = re.compile(r"\s-o\s+(?:lib|sdk)/build/([\w.-]+?)\.(?:o|tmp)\b")
RE_PLUG_DEC = re.compile(r"\s-o\s+([\w.-]+)/build/decoder\.tmp\b")
RE_BUILT_PLUGIN = re.compile(r"^built plugin ([\w.-]+)")
liquid_log = os.path.join(K, "liquid-dsp", "build-kraken.log")
liquid_pos = [0]

def poll_liquid():
    it = items.get("liquid")
    if not it or it.state != "run":
        return
    try:
        with open(liquid_log, "rb") as f:
            f.seek(liquid_pos[0])
            data = f.read()
            liquid_pos[0] += len(data)
    except OSError:
        return
    for line in data.decode("utf-8", "replace").splitlines():
        m = RE_PCT.match(line)
        if m:
            it.frac = max(it.frac, int(m.group(1)) / 100.0)
            state["step"] = "liquid-dsp %d%%" % int(m.group(1))

def on_line(app, line):
    s = line.strip()
    if app == "heimdall" or app == "kraken_doa":
        if "Installing uWebSockets" in s:
            start(unplanned(app + ":uws-clone", app, app + ": uWebSockets", W_CLONE, "Fetching uWebSockets", "explicit"))
            return
        if s.startswith("Building uSockets"):
            finish_where(lambda it: it.key == app + ":uws-clone")
            start(unplanned(app + ":usockets", app, app + ": uWebSockets", W_USOCKETS, "Building uSockets", "explicit"))
            return
        if "uWebSockets found locally" in s or ("Leaving directory" in s and "uSockets" in s):
            finish_where(lambda it: it.key in (app + ":uws-clone", app + ":usockets"))
            return
    if app == "heimdall":
        if "librtlsdr fork not found - cloning" in s:
            start(unplanned("rtl-clone", app, "heimdall: librtlsdr", W_CLONE, "Fetching the librtlsdr fork", "explicit"))
            return
        if s.startswith("cmake -S ../librtlsdr"):
            finish_where(lambda it: it.key == "rtl-clone")
            start(unplanned("rtl", app, "heimdall: librtlsdr", W_RTL, "Building the librtlsdr fork", "explicit"))
            return
        it = items.get("rtl")
        if it and it.state == "run":
            m = RE_PCT.match(s)
            if m:
                it.frac = max(it.frac, int(m.group(1)) / 100.0)
                state["step"] = "librtlsdr " + s
                if "Built target rtlsdr_static" in s:
                    finish(it)
                return
        m = RE_COMPILING.match(s)
        if m:
            start(unplanned("h:" + m.group(1), app, "heimdall", W_HOBJ, "Compiling " + m.group(1)))
            return
        if s.startswith("Linking heimdall"):
            finish_where(lambda it: it.app == app and it.key not in ("h:link",))
            start(unplanned("h:link", app, "heimdall", W_LINK, "Linking heimdall", "explicit"))
            return
        if s.startswith("Build complete"):
            finish_where(lambda it: it.key == "h:link")
        return
    # kraken_doa (+ the plugins' make, which shares its output)
    if s.startswith("liquid-dsp: building"):
        liquid_pos[0] = 0
        start(unplanned("liquid", app, "kraken_doa: liquid-dsp", W_LIQUID, "Building liquid-dsp", "explicit"))
        return
    if s.startswith("liquid-dsp: ") and "objects)" in s:
        finish_where(lambda it: it.key == "liquid")
        return
    m = RE_COMPILING.match(s)
    if m:
        start(unplanned("k:" + m.group(1), app, "kraken_doa", W_KOBJ, "Compiling " + m.group(1)))
        return
    if s.startswith("Linking kraken_doa"):
        finish_where(lambda it: it.key.startswith("k:") and it.key != "k:link")
        start(unplanned("k:link", app, "kraken_doa", W_LINK, "Linking kraken_doa", "explicit"))
        return
    if s.startswith("Build complete!"):
        finish_where(lambda it: it.key == "k:link")
        return
    m = RE_BUILT_PLUGIN.match(s)
    if m:
        finish_where(lambda it, n=m.group(1): it.key == "p:" + n)
        return
    if "ar rcs lib/build/libkrakendig.a" in s:
        finish_where(lambda it: it.key.startswith("p:lib/") or it.key == "p:host")
        return
    m = RE_PLUG_DEC.search(s)
    if m:
        start(unplanned("p:" + m.group(1), app, "decoder plugins", W_PLUGIN_BASE + 50 * W_PLUGIN_PER_KB,
                        "Building plugin " + m.group(1)))
        return
    m = RE_PLUG_LIB.search(s)
    if m:
        n = m.group(1)
        key = "p:host" if n == "plugin_host" else ("p:codecs" if n == "codecs" else "p:lib/" + n)
        label = {"p:host": "Compiling the plugin host", "p:codecs": "Building the codec check"}.get(key, "Compiling plugins/lib " + n)
        start(unplanned(key, app, "decoder plugins", W_LIBOBJ, label))
        return

# ---- error summary ----
RE_DEP = [
    (re.compile(r"fatal error: ([^:]+): No such file or directory"), "a header is missing: %s"),
    (re.compile(r"cannot find -l(\S+)"), "a library is missing: lib%s"),
    (re.compile(r"(?:^|\s)([\w.+-]+): (?:command )?not found"), "a build tool is missing: %s"),
    (re.compile(r"Package '?([\w.+-]+)'? was not found"), "a pkg-config package is missing: %s"),
    (re.compile(r"Could NOT find (\w+)"), "CMake could not find %s"),
    (re.compile(r"Eigen3 not found"), "Eigen3 is missing%s"),
]
RE_NET = re.compile(r"Could not resolve host|unable to access|could not fetch|Failed to connect|Connection timed out|Network is unreachable", re.I)
RE_ERR = re.compile(r"(fatal error|error:|Error \d+|\berror\b|could not|cannot)", re.I)

def summarize_error(lines, app):
    for rx, fmt in RE_DEP:
        for l in lines:
            m = rx.search(l)
            if m:
                arg = m.group(1) if m.groups() else ""
                return ("%s build failed - " % app) + (fmt % arg) + " - " + INSTALL_HINT
    for l in lines:
        if RE_NET.search(l):
            return "%s build failed - a download failed (network needed for the first build): %s" % (app, l.strip()[:300])
    for l in lines:
        if re.search(r"(fatal error|\berror:)", l):
            return "%s build failed: %s" % (app, l.strip()[:300])
    for l in lines:
        if RE_ERR.search(l) and not l.startswith("make") :
            return "%s build failed: %s" % (app, l.strip()[:300])
    for l in reversed(lines):
        if l.strip():
            return "%s build failed: %s" % (app, l.strip()[:300])
    return "%s build failed" % app

# ---- run ----
logf = None
current = {"proc": None}
interrupted = {"sig": None}

def emit(text):
    try:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()
    except (OSError, ValueError):
        pass
    if logf:
        try:
            logf.write(text + "\n")
            logf.flush()
        except (OSError, ValueError):
            pass

def fail(msg, code=1):
    state["state"] = "failed"
    state["error"] = msg
    state["step"] = ""
    stop_ticker.set()
    write_status()
    sys.stderr.write("autobuild: %s\n" % msg)
    sys.stderr.flush()
    if logf:
        logf.write("autobuild: %s\n" % msg)
        logf.flush()
    sys.exit(code)

def on_signal(signum, frame):
    interrupted["sig"] = signum
    p = current["proc"]
    if p and p.poll() is None:
        try:
            p.send_signal(signal.SIGTERM)   # make passes it on to its jobs
        except OSError:
            pass
for _s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(_s, on_signal)

def main():
    global logf
    try:
        logf = open(LOG, "w", encoding="utf-8", errors="replace")
    except OSError as e:
        sys.stderr.write("autobuild: cannot write the log %s: %s\n" % (LOG, e))
    write_status()
    if not APPS:
        emit("autobuild: heimdall and kraken_doa are up to date - nothing to build")
        state.update(state="done", percent=100, phase="done", step="nothing to build")
        write_status()
        return 0
    for line in WHY.splitlines():
        emit("autobuild: " + line)
    missing = [t for t in ("make", "g++", "gcc", "cmake", "git", "pkg-config") if not shutil.which(t)]
    if missing:
        fail("missing build tools: %s - %s" % (" ".join(missing), INSTALL_HINT))
    for a in APPS:
        if a == "heimdall":
            plan_heimdall()
        else:
            plan_kraken()
    emit("autobuild: building %s with %d parallel job%s (%d steps planned; log: %s)"
         % (" and ".join(APPS), JOBS, "" if JOBS == 1 else "s", len(order), LOG))
    th = threading.Thread(target=ticker, daemon=True)
    th.start()
    env = dict(os.environ)
    # English tool messages (the parser reads them) without touching LC_CTYPE
    env.pop("LANGUAGE", None)
    if "LC_ALL" in env:
        env["LC_CTYPE"] = env.pop("LC_ALL")
    env["LC_MESSAGES"] = "C"
    warnings = 0
    for a in APPS:
        t_app = time.time()
        state["phase"] = a
        state["step"] = "make -j%d in %s" % (JOBS, os.path.basename(APP_DIR[a]))
        emit("autobuild: === %s: make -j%d (in %s) ===" % (a, JOBS, os.path.relpath(APP_DIR[a], ROOT)))
        recent = deque(maxlen=400)
        try:
            p = subprocess.Popen(["make", "-j%d" % JOBS], cwd=APP_DIR[a], env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except OSError as e:
            fail("cannot run make: %s - %s" % (e, INSTALL_HINT))
        current["proc"] = p
        for raw in iter(p.stdout.readline, b""):
            line = CTRL.sub("?", raw.decode("utf-8", "replace").rstrip("\r\n"))
            emit(line)
            recent.append(line)
            with status_lock:
                tail.append(line[:500])
                try:
                    on_line(a, line)
                except Exception as e:   # the progress is cosmetic: never fail the build on it
                    sys.stderr.write("autobuild: progress parser: %r\n" % e)
            if line.startswith("WARNING: a decoder plugin failed"):
                warnings += 1
        rc = p.wait()
        current["proc"] = None
        if interrupted["sig"]:
            fail("build interrupted (signal %d)" % interrupted["sig"], 128 + interrupted["sig"])
        exe = os.path.join(APP_DIR[a], a)
        if rc != 0 or not os.path.isfile(exe):
            fail(summarize_error(list(recent), a) if rc != 0 else "%s build finished but %s is missing" % (a, exe))
        with status_lock:
            finish_where(lambda it, a=a: it.app == a)
        # sources older than this build's start are now built (check's reference)
        try:
            os.makedirs(os.path.dirname(OK_STAMP[a]), exist_ok=True)
            with open(OK_STAMP[a], "w") as f:
                f.write("%d\n" % int(t_app))
            os.utime(OK_STAMP[a], (t_app, t_app))
        except OSError:
            pass
        emit("autobuild: %s built in %d s" % (a, time.time() - t_app))
    stop_ticker.set()
    th.join(2)
    state.update(state="done", percent=100, phase="done",
                 step="build complete" + (" (a decoder plugin failed to build - see the log)" if warnings else ""))
    write_status()
    emit("autobuild: done in %d s" % (time.time() - t_start))
    return 0

sys.exit(main())
PY
