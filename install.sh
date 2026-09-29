#!/usr/bin/env bash
#
# install.sh - Install dependencies and build both KrakenSDR v2 applications.
#
#   * Heimdall server   (heimdall_v2/heimdall)      - coherent multi-RTL-SDR receiver
#   * KrakenSDR DoA      (kraken_doa_v2/kraken_doa)  - FFT viewer / MUSIC DoA client
#
# IMPORTANT: KrakenSDR needs the krakenrf-specific librtlsdr fork
# (https://github.com/krakenrf/librtlsdr), NOT the stock distro librtlsdr.
# This script builds that fork from source; heimdall links it STATICALLY, so
# the distro rtl-sdr / librtlsdr0 packages - and everything that depends on
# them (gqrx, gr-osmosdr, rtl_433, ...) - can stay installed alongside it.
# The fork is also installed to /usr/local (library, headers, and rtl_* tools
# linked statically against it) for other software that wants it.
#
# Also installs gpsd (the DoA client reads it via JSON on port 2947) and
# generates a self-signed SSL certificate for the DoA client's HTTPS web UI
# (kraken_doa_v2/server.crt + server.key) if one is not already present.
#
# Usage:
#   ./install.sh                # install deps + librtlsdr fork + build both apps
#   JOBS=2 ./install.sh         # override parallel build jobs (default: by RAM, 1-3)
#   CLEAN=1 ./install.sh        # force a clean rebuild of the apps
#   SKIP_DEPS=1 ./install.sh    # skip apt install (still builds librtlsdr + apps)
#   SKIP_RTLSDR=1 ./install.sh  # skip the librtlsdr fork build/install (heimdall's
#                               # make still builds the static library if missing)
#
set -euo pipefail

# On any failure, say exactly where we died - with `set -e` a mid-script abort
# otherwise looks like steps were silently skipped.
trap 'echo -e "\n\033[31m\033[1minstall.sh ABORTED\033[0m at line $LINENO: $BASH_COMMAND" >&2' ERR

# Resolve the directory this script lives in so it works from anywhere.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HEIMDALL_DIR="$SCRIPT_DIR/heimdall_v2"
KRAKEN_DIR="$SCRIPT_DIR/kraken_doa_v2"
LIBRTLSDR_DIR="$SCRIPT_DIR/librtlsdr"
LIBRTLSDR_REPO="https://github.com/krakenrf/librtlsdr"

# Build parallelism. Each g++ job on the Eigen-heavy files needs ~1 GB+ at -O3,
# so scale the default to the machine's RAM: a 2 GB Pi 4 OOM-kills cc1plus even
# at -j3, while an 8 GB Pi 5 handles -j3 fine (-j4 crashes: all cores busy).
if [[ -z "${JOBS:-}" ]]; then
    MEM_MB="$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)"
    if   (( MEM_MB >= 7000 )); then JOBS=3
    elif (( MEM_MB >= 3500 )); then JOBS=2
    else                            JOBS=1
    fi
fi

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

# --- sanity checks ----------------------------------------------------------
[[ -d "$HEIMDALL_DIR" ]] || die "heimdall_v2/ not found next to this script ($HEIMDALL_DIR)"
[[ -d "$KRAKEN_DIR"   ]] || die "kraken_doa_v2/ not found next to this script ($KRAKEN_DIR)"

# --- 1. system dependencies -------------------------------------------------
# NOTE: librtlsdr-dev and rtl-sdr are not needed - we build the krakenrf fork
# from source below instead (installed distro copies are left alone). Notes on
# the rest:
#   libusb-1.0-0-dev/libusb-dev - required to build librtlsdr
#   libopus-dev  - kraken links -lopus (missing from its `make deps`, added here)
#   zlib1g-dev   - both link -lz
#   libsqlite3-dev - kraken links -lsqlite3
#   tmux         - used by run.sh for the split-screen dashboards
PACKAGES=(
    build-essential cmake git pkg-config     # toolchain (both apps + librtlsdr)
    libusb-1.0-0-dev libusb-dev              # USB backend for librtlsdr fork
    libfftw3-dev                             # FFT (both, single-precision fftw3f)
    libeigen3-dev                            # eigenvalue decomposition (both)
    libssl-dev libcrypto++-dev               # SSL for uWebSockets (both)
    libliquid-dev                            # DSP: decimation/resampling (kraken)
    libopus-dev                              # Opus audio codec (kraken)
    zlib1g-dev                               # zlib -lz (both)
    libsqlite3-dev                           # settings store (kraken)
    tmux                                     # split-screen dashboards (run.sh)
    gpsd gpsd-tools                          # GPS daemon (kraken talks JSON to port 2947; tools = cgps/gpsmon for debugging)
    openssl                                  # self-signed cert generation for the kraken web UI
)

if [[ "${SKIP_DEPS:-0}" == "1" ]]; then
    say "Skipping apt dependency install (SKIP_DEPS=1)"
elif command -v apt-get >/dev/null 2>&1; then
    say "Installing system dependencies (apt)"
    sudo apt-get update
    sudo apt-get install -y "${PACKAGES[@]}"
    ok "Dependencies installed"
else
    warn "apt-get not found - this system is not Debian/Ubuntu."
    warn "Install the equivalents of these packages manually, then re-run with SKIP_DEPS=1:"
    warn "  ${PACKAGES[*]}"
    die "Cannot auto-install dependencies on a non-apt system."
fi

# --- 2. KrakenSDR librtlsdr fork --------------------------------------------
install_librtlsdr() {
    say "Installing KrakenSDR librtlsdr fork"

    # Distro RTL-SDR packages are left installed. They used to be purged
    # (taking gqrx, gr-osmosdr, etc. with them) because their stock
    # librtlsdr.so.0 is found before /usr/local/lib by the loader and would
    # shadow the fork. heimdall now links the fork statically and the rtl_*
    # tools below are linked statically too, so nothing of ours can pick up
    # the stock library.

    # Fetch (or update) the fork. librtlsdr/ is NOT tracked by this repo
    # (gitignored) - this script provides it. Three possible states:
    #   1. proper git clone            -> pull and build
    #   2. source tree without .git    -> build it as-is (e.g. unpacked from a
    #      tarball; `git clone` into a non-empty dir would abort the script)
    #   3. missing or empty dir        -> fresh clone
    if [[ -d "$LIBRTLSDR_DIR/.git" ]]; then
        ok "Reusing existing clone: ${LIBRTLSDR_DIR#$SCRIPT_DIR/}"
        git -C "$LIBRTLSDR_DIR" pull --ff-only || warn "git pull failed; building the current checkout"
    elif [[ -f "$LIBRTLSDR_DIR/CMakeLists.txt" ]]; then
        ok "Using existing librtlsdr source tree (no .git; building as-is)"
    else
        rm -rf "$LIBRTLSDR_DIR"   # drop the empty placeholder dir a repo checkout leaves
        git clone "$LIBRTLSDR_REPO" "$LIBRTLSDR_DIR"
    fi

    # Build and install to /usr/local, with udev rules for non-root USB access.
    # build/src/librtlsdr.a is what heimdall links (see heimdall_v2/Makefile).
    rm -rf "$LIBRTLSDR_DIR/build"
    mkdir -p "$LIBRTLSDR_DIR/build"
    pushd "$LIBRTLSDR_DIR/build" >/dev/null
    cmake ../ -DCMAKE_BUILD_TYPE=Release -DINSTALL_UDEV_RULES=ON \
        -DLINK_RTLTOOLS_AGAINST_STATIC_LIB=ON
    make -j"$JOBS"
    sudo make install
    sudo ldconfig
    popd >/dev/null

    # Blacklist the DVB-T kernel driver so it does not grab the dongles.
    local bl=/etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf
    if ! grep -qs 'dvb_usb_rtl28xxu' "$bl" 2>/dev/null; then
        echo 'blacklist dvb_usb_rtl28xxu' | sudo tee "$bl" >/dev/null
        ok "Blacklisted dvb_usb_rtl28xxu (unload now with: sudo modprobe -r dvb_usb_rtl28xxu)"
    fi
    ok "librtlsdr fork installed to /usr/local (ldconfig refreshed)"
}

# Realtime scheduling for the sample pipeline: heimdall raises its USB reader
# and sample-drain threads to SCHED_RR 30 (best-effort) for USB keep-up under
# load. Without an rtprio limit it silently runs at normal priority instead.
# Only these two thread classes are meant to be realtime - do NOT run the whole
# process under chrt: the conversion worker is deliberately normal priority
# (its malloc/mutex use would priority-invert against the web/FFTW threads).
install_rtprio_limit() {
    local user="${SUDO_USER:-$USER}"
    local conf=/etc/security/limits.conf
    if ! grep -Eqs "^[[:space:]]*${user}[[:space:]].*rtprio" "$conf" 2>/dev/null; then
        echo "${user} - rtprio 30" | sudo tee -a "$conf" >/dev/null
        ok "Granted rtprio 30 to '${user}' in limits.conf (takes effect on next login)"
    else
        ok "rtprio limit for '${user}' already present in limits.conf"
    fi
}

# KrakenSDR Wideband variant: heimdall --wideband programs the downconverter LO
# (a moRFeus box, USB HID 10c4:eac9) via /dev/hidraw*. Grant non-root access.
install_morfeus_udev_rule() {
    local rule=/etc/udev/rules.d/99-morfeus.rules
    if [[ ! -f "$rule" ]]; then
        echo 'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="eac9", MODE="0666"' \
            | sudo tee "$rule" >/dev/null
        sudo udevadm control --reload-rules 2>/dev/null || true
        sudo udevadm trigger 2>/dev/null || true
        ok "Installed moRFeus udev rule (for KrakenSDR Wideband --wideband mode)"
    fi
}

if [[ "${SKIP_RTLSDR:-0}" == "1" ]]; then
    say "Skipping librtlsdr fork build (SKIP_RTLSDR=1)"
else
    install_librtlsdr
fi
install_morfeus_udev_rule
install_rtprio_limit

# --- 3. build both applications ---------------------------------------------
# (build AFTER librtlsdr so heimdall links the freshly built fork's static library)
build_app() {
    local name="$1" dir="$2" binary="$3"
    say "Building ${name}"
    pushd "$dir" >/dev/null
    if [[ "${CLEAN:-0}" == "1" ]]; then
        warn "CLEAN=1 - running 'make clean' first"
        make clean >/dev/null 2>&1 || true
    fi
    # `make` auto-clones/builds the app's local uWebSockets on first run.
    make -j"$JOBS"
    popd >/dev/null
    [[ -x "$dir/$binary" ]] || die "${name} build finished but '$binary' is missing"
    ok "${name} built -> ${dir#$SCRIPT_DIR/}/$binary"
}

build_app "Heimdall server" "$HEIMDALL_DIR" "heimdall"
build_app "KrakenSDR DoA client" "$KRAKEN_DIR" "kraken_doa"

# --- 4. SSL certificate for the DoA client web UI ----------------------------
# kraken_doa serves HTTPS/WSS on port 8080 and loads server.crt/server.key from
# its working directory (SSL_CERT_FILE/SSL_KEY_FILE in include/config.hpp).
install_kraken_certs() {
    local crt="$KRAKEN_DIR/server.crt" key="$KRAKEN_DIR/server.key"
    if [[ -s "$crt" && -s "$key" ]]; then
        # The files existing is not enough: the cert must actually belong to
        # the key, or the web server's SSL context (and its listen) fails.
        if [[ "$(openssl x509 -noout -pubkey -in "$crt" 2>/dev/null)" == \
              "$(openssl pkey -in "$key" -pubout 2>/dev/null)" ]]; then
            ok "SSL certificate already present: kraken_doa_v2/server.crt"
            return
        fi
        warn "server.crt does not match server.key - regenerating the pair"
    fi
    say "Generating self-signed SSL certificate for the DoA client web UI"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$key" -out "$crt" -subj "/CN=krakensdr" 2>/dev/null
    ok "Created kraken_doa_v2/server.crt + server.key (self-signed, 10 years)"
}
install_kraken_certs

# --- 5. git secret guards ----------------------------------------------------
# Auto-sanitize station secrets (KrakenPro key, lat/lon) so they can never be
# committed/pushed: a clean filter (.gitattributes -> scripts/git-secrets-clean)
# zeroes the sensitive fields in what git stores while the working file keeps
# its real values, and .githooks/pre-push blocks any push whose outgoing
# commits still contain them. Filters/hooks are per-clone config (git does not
# version them), which is why this runs here.
setup_git_guards() {
    [[ -d "$SCRIPT_DIR/.git" ]] || return 0
    git -C "$SCRIPT_DIR" config filter.kraken-secrets.clean 'scripts/git-secrets-clean'
    git -C "$SCRIPT_DIR" config filter.kraken-secrets.required true
    git -C "$SCRIPT_DIR" config core.hooksPath .githooks
    ok "Git guards: secrets clean-filter + pre-push scan configured"
}
setup_git_guards

# --- done -------------------------------------------------------------------
echo
say "${GRN}Install complete.${RST}"
echo "   Start everything with:  ${BOLD}${SCRIPT_DIR}/run.sh${RST}"
echo
warn "RTL-SDR USB permissions: the udev rules were installed, but if Heimdall still"
echo "     reports no devices, add yourself to plugdev and re-plug / re-login:"
echo "       sudo usermod -aG plugdev \"\$USER\""
warn "Realtime scheduling (rtprio) was granted in limits.conf but only applies to"
echo "     NEW logins - log out and back in (or reboot) before the first run."
echo "     Verify with:  rtl_test -t   (rtl_test ships with the fork's rtl-tools)"
