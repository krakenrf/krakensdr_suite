#!/usr/bin/env bash
#
# windows/build.sh - build the KrakenSDR v2 suite on Windows (the install.sh of
# the Windows build). Runs in MSYS2's UCRT64 environment - build_windows.bat at
# the repository root installs MSYS2 when needed and starts this script; from
# an "MSYS2 UCRT64" shell it can also be run directly:
#
#   windows/build.sh              # MSYS2 packages + librtlsdr fork + both apps
#                                 # + decoder plugins + krakensdr.exe launcher
#   JOBS=4 windows/build.sh       # parallel build jobs (default: all CPUs)
#   CLEAN=1 windows/build.sh      # clean rebuild of the apps
#   SKIP_DEPS=1 windows/build.sh  # skip the pacman package install
#   WHISPER=1 windows/build.sh    # also set up Voice to Text (whisper.cpp + a model)
#
# Result (all self-contained .exe files - static, no MSYS2 DLLs needed):
#   heimdall_v2\heimdall.exe, kraken_doa_v2\kraken_doa.exe,
#   kraken_doa_v2\plugins\<id>\build\decoder.exe, krakensdr.exe (starts both;
#   run.bat does the same), librtlsdr\build\src\rtl_test.exe / rtl_eeprom.exe
# windows/package.sh then copies what is needed to run into dist\ (a folder you
# can zip and copy to another PC).
set -Eeuo pipefail
trap 'echo -e "\n\033[31m\033[1mbuild.sh ABORTED\033[0m at line $LINENO: $BASH_COMMAND" >&2' ERR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEIMDALL_DIR="$ROOT/heimdall_v2"
KRAKEN_DIR="$ROOT/kraken_doa_v2"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

if [[ -t 1 ]]; then
    BOLD=$'\033[1m'; GRN=$'\033[32m'; YEL=$'\033[33m'; RED=$'\033[31m'; CYN=$'\033[36m'; RST=$'\033[0m'
else
    BOLD=""; GRN=""; YEL=""; RED=""; CYN=""; RST=""
fi
say()  { echo "${CYN}${BOLD}==>${RST} ${BOLD}$*${RST}"; }
ok()   { echo "${GRN}   ✓ $*${RST}"; }
warn() { echo "${YEL}   ! $*${RST}"; }
die()  { echo "${RED}${BOLD}Error:${RST} $*" >&2; exit 1; }

# the native MinGW-w64 toolchain, not MSYS2's own (Cygwin-like) gcc
if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
    die "run this in MSYS2's UCRT64 environment (build_windows.bat does), not '${MSYSTEM:-plain bash}'"
fi

# --- 1. packages ------------------------------------------------------------
#   gcc / make / cmake / ninja / pkgconf - the toolchain (apps, librtlsdr,
#     liquid-dsp, uSockets)
#   libusb - librtlsdr (heimdall) reaches the dongles through WinUSB with it
#   libuv - uSockets' event loop on Windows (no epoll)
#   eigen3 (both apps), openssl (uWS TLS, web mapper), opus, zlib, sqlite3
#   dlfcn - dlopen() for the optional mbelib DLL (DMR / D-STAR / NXDN voice)
PACKAGES=(
    make git patch diffutils
    mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
    mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-libuv
    mingw-w64-ucrt-x86_64-eigen3 mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-opus
    mingw-w64-ucrt-x86_64-zlib mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-dlfcn
)
if [[ "${SKIP_DEPS:-0}" == "1" ]]; then
    say "Skipping the MSYS2 package install (SKIP_DEPS=1)"
else
    say "Installing MSYS2 packages (pacman)"
    pacman -S --needed --noconfirm "${PACKAGES[@]}" >/dev/null
    ok "Toolchain and libraries installed"
fi

# --- 2. both applications ------------------------------------------------------
# heimdall's make clones + builds the librtlsdr fork's static library
# (../librtlsdr) and both apps clone + build their pinned uWebSockets;
# kraken_doa's builds liquid-dsp (liquid.mk) and the decoder plugins.
build_app() {
    local name="$1" dir="$2" binary="$3"
    say "Building ${name}"
    pushd "$dir" >/dev/null
    if [[ "${CLEAN:-0}" == "1" ]]; then
        warn "CLEAN=1 - running 'make clean' first"
        make clean >/dev/null 2>&1 || true
    fi
    make -j"$JOBS"
    popd >/dev/null
    [[ -f "$dir/$binary" ]] || die "${name} build finished but '$binary' is missing"
    ok "${name} built -> ${dir#$ROOT/}/$binary"
}
build_app "Heimdall server" "$HEIMDALL_DIR" "heimdall.exe"
build_app "KrakenSDR DoA client (+ decoder plugins)" "$KRAKEN_DIR" "kraken_doa.exe"

# Voice to Text (optional, as install.sh's WHISPER=1): whisper.cpp built as
# transcribe/build/kraken_whisper.exe + the default model; afterwards every
# `make` keeps it up to date
if [[ "${WHISPER:-0}" == "1" ]]; then
    say "Setting up Voice to Text (whisper.cpp + the base.en model)"
    make -C "$KRAKEN_DIR" whisper
    ok "Voice to Text ready -> kraken_doa_v2/transcribe/"
fi

# the fork's rtl_test / rtl_eeprom (check the dongles: rtl_test -t), static
# like the apps - heimdall's make configures librtlsdr/build with the static
# libusb (and configures a build dir from before that again)
say "Building rtl_test / rtl_eeprom"
make -C "$ROOT/librtlsdr/build" -j"$JOBS" rtl_test rtl_eeprom >/dev/null
for t in rtl_test rtl_eeprom; do
    if objdump -p "$ROOT/librtlsdr/build/src/$t.exe" | grep -i 'DLL Name: libusb' >/dev/null; then
        warn "$t.exe needs libusb-1.0.dll (runs in the MSYS2 shell only) - delete librtlsdr/build and build again"
    fi
done
ok "librtlsdr/build/src/rtl_test.exe, rtl_eeprom.exe"

# --- 3. the launcher -----------------------------------------------------------
say "Building krakensdr.exe (starts both apps - the run.sh of Windows)"
make -C "$ROOT/windows" >/dev/null
ok "krakensdr.exe built"

# --- 4. TLS certificate for the DoA client's web UI ----------------------------
# kraken_doa serves HTTPS/WSS on 8080 from server.crt / server.key in its
# folder. It creates a self-signed pair itself when there is none; making it
# here too gives the same files as install.sh on Linux.
if [[ ! -s "$KRAKEN_DIR/server.crt" || ! -s "$KRAKEN_DIR/server.key" ]] && command -v openssl >/dev/null 2>&1; then
    say "Generating a self-signed SSL certificate for the DoA client web UI"
    MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$KRAKEN_DIR/server.key" -out "$KRAKEN_DIR/server.crt" -subj "/CN=krakensdr" 2>/dev/null &&
        ok "Created kraken_doa_v2/server.crt + server.key (self-signed, 10 years)" ||
        warn "openssl failed - kraken_doa will create the certificate at its first start"
fi
# The private key readable by this account (+ SYSTEM, Administrators) only,
# as the 0600 it gets on Linux: Windows ignores the mode openssl asks for, so
# the key took the folder's permissions - outside the user profile every
# local account could read and replace it. Also a key from an earlier build
# or from kraken_doa's own first start. The account's SID, not %USERNAME%
# (which can be ambiguous or empty); MSYS2_ARG_CONV_EXCL: MSYS2 would turn
# the /options into paths
if [[ -f "$KRAKEN_DIR/server.key" ]]; then
    SYSDIR="$(cygpath -S)"
    SID="$(MSYS2_ARG_CONV_EXCL='*' "$SYSDIR/whoami.exe" /user /fo csv /nh 2>/dev/null | tr -d '\r' |
           awk -F'","' '{ gsub(/"/, "", $2); print $2 }')" || SID=""
    if [[ "$SID" != S-1-* ]]; then
        warn "could not read this account's SID - server.key keeps its folder's permissions"
    elif ! MSYS2_ARG_CONV_EXCL='*' "$SYSDIR/icacls.exe" "$(cygpath -w "$KRAKEN_DIR/server.key")" /inheritance:r \
            /remove:g '*S-1-1-0' '*S-1-5-11' '*S-1-5-32-545' \
            /grant:r "*$SID:F" '*S-1-5-18:F' '*S-1-5-32-544:F' >/dev/null; then
        warn "icacls failed - server.key keeps its folder's permissions"
    fi
fi

# --- 5. git secret guards (as install.sh) ------------------------------------
if [[ -d "$ROOT/.git" || -f "$ROOT/.git" ]]; then
    git -C "$ROOT" config filter.kraken-secrets.clean 'scripts/git-secrets-clean'
    git -C "$ROOT" config filter.kraken-secrets.required true
    git -C "$ROOT" config core.hooksPath .githooks
    ok "Git guards: secrets clean-filter + pre-push scan configured"
fi

echo
say "${GRN}Build complete.${RST}"
echo "   Start everything with:  ${BOLD}run.bat${RST}  (or krakensdr.exe; krakensdr.exe stop stops it)"
echo
warn "Each KrakenSDR tuner needs the WinUSB driver once (Zadig, https://zadig.akeo.ie):"
echo "     Options > List All Devices, pick each 'Bulk-In, Interface (Interface 0)' with USB ID"
echo "     0BDA 2838, driver WinUSB, Replace Driver (5 times for a KrakenSDR). See WINDOWS.md."
echo "     Check with: librtlsdr\\build\\src\\rtl_test.exe -t"
