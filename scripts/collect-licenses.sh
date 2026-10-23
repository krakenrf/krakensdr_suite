#!/usr/bin/env bash
#
# scripts/collect-licenses.sh DEST - put the licence files a binary
# distribution of the suite has to carry into DEST (run by windows/package.sh
# and the Dockerfile):
#
#   DEST/LICENSE, DEST/heimdall_v2/, DEST/kraken_doa_v2/
#                         the suite's own licences + THIRD_PARTY_NOTICES.md;
#                         kraken_doa_v2/plugins/sdk/LICENSE (the plugin SDK,
#                         MIT) and kraken_doa_v2/plugins/<id>/LICENSE of every
#                         decoder plugin under a licence of its own
#   DEST/third-party/<component>/
#                         the licence files of what the binaries contain:
#                         pffft, concurrentqueue and - where their source
#                         trees are found - the librtlsdr fork, uWebSockets /
#                         uSockets, liquid-dsp, whisper.cpp; in MSYS2
#                         (MINGW_PREFIX set) the licences of the MSYS2
#                         libraries the static .exe files link; on Debian the
#                         copyright file of Eigen (headers compiled in - the
#                         runtime packages keep their own in /usr/share/doc)
#   DEST/SOURCE.txt       where the corresponding source of the GPL programs
#                         (heimdall, the librtlsdr tools) is
#
#   scripts/collect-licenses.sh DEST [rtlsdr=DIR] [uws=DIR] [liquid=DIR]
#                                    [whisper=DIR] [msys2=PREFIX]
#
# The source trees default to ROOT/librtlsdr, kraken_doa_v2/uWebSockets,
# kraken_doa_v2/liquid-dsp and kraken_doa_v2/transcribe/whisper.cpp; a tree
# that isn't there is left out (e.g. no whisper.cpp when Voice to Text isn't
# built). msys2= defaults to $MINGW_PREFIX (set in an MSYS2 shell).
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:?usage: scripts/collect-licenses.sh DEST [rtlsdr=DIR] [uws=DIR] [liquid=DIR] [whisper=DIR] [msys2=PREFIX]}"
shift
RTLSDR_DIR="$ROOT/librtlsdr"
UWS_DIR="$ROOT/kraken_doa_v2/uWebSockets"
LIQUID_DIR="$ROOT/kraken_doa_v2/liquid-dsp"
WHISPER_DIR="$ROOT/kraken_doa_v2/transcribe/whisper.cpp"
MSYS2="${MINGW_PREFIX:-}"
for a in "$@"; do
    case $a in
        rtlsdr=*) RTLSDR_DIR=${a#*=} ;;
        uws=*) UWS_DIR=${a#*=} ;;
        liquid=*) LIQUID_DIR=${a#*=} ;;
        whisper=*) WHISPER_DIR=${a#*=} ;;
        msys2=*) MSYS2=${a#*=} ;;
        *) echo "collect-licenses.sh: unknown argument '$a'" >&2; exit 2 ;;
    esac
done

mkdir -p "$DEST/heimdall_v2" "$DEST/kraken_doa_v2" "$DEST/third-party"
cp "$ROOT/LICENSE" "$DEST/"
cp "$ROOT/heimdall_v2/LICENSE" "$ROOT/heimdall_v2/THIRD_PARTY_NOTICES.md" "$DEST/heimdall_v2/"
cp "$ROOT/kraken_doa_v2/LICENSE" "$ROOT/kraken_doa_v2/THIRD_PARTY_NOTICES.md" "$ROOT/kraken_doa_v2/COMMERCIAL_TERMS.md" \
   "$DEST/kraken_doa_v2/"
# the plugin SDK (MIT: in every plugin) and the plugins with a LICENSE of
# their own - each is under that licence, not kraken_doa_v2's
mkdir -p "$DEST/kraken_doa_v2/plugins/sdk"
cp "$ROOT/kraken_doa_v2/plugins/sdk/LICENSE" "$DEST/kraken_doa_v2/plugins/sdk/"
GPL_PLUGINS=
for f in "$ROOT"/kraken_doa_v2/plugins/*/LICENSE; do
    [[ -f $f ]] || continue
    id=$(basename "$(dirname "$f")")
    [[ $id == sdk ]] && continue
    mkdir -p "$DEST/kraken_doa_v2/plugins/$id"
    cp "$f" "$DEST/kraken_doa_v2/plugins/$id/"
    if grep -q 'GNU GENERAL PUBLIC LICENSE' "$f"; then
        printf -v line '  %-32s%s\n' "decoder plugin $id" "kraken_doa_v2/plugins/$id/ in the suite above, same commit"
        GPL_PLUGINS+=$line
    fi
done

# put NAME FILE... - the FILEs that exist into third-party/NAME/
put() {
    local name=$1 f
    shift
    for f in "$@"; do
        [[ -f $f ]] || continue
        mkdir -p "$DEST/third-party/$name"
        cp "$f" "$DEST/third-party/$name/"
    done
}
put pffft "$ROOT/kraken_doa_v2/external/pffft/LICENSE.txt"
put concurrentqueue "$ROOT/heimdall_v2/external/concurrentqueue/LICENSE.md"
put librtlsdr "$RTLSDR_DIR/COPYING" "$RTLSDR_DIR/AUTHORS"
put uWebSockets "$UWS_DIR/LICENSE"
put uSockets "$UWS_DIR/uSockets/LICENSE"
if [[ -f $UWS_DIR/src/MoveOnlyFunction.h ]]; then   # MIT, its notice is the file's first comment
    sed -n '1,/\*\//p' "$UWS_DIR/src/MoveOnlyFunction.h" > "$DEST/third-party/uWebSockets/MoveOnlyFunction-LICENSE.txt"
fi
put liquid-dsp "$LIQUID_DIR/LICENSE"
put whisper.cpp "$WHISPER_DIR/LICENSE"

if [[ -n $MSYS2 && -d $MSYS2/share/licenses ]]; then
    # what the static Windows programs link from MSYS2
    for p in crt winpthreads libwinpthread libgcc libstdc++ libusb libuv openssl opus zlib sqlite3 eigen dlfcn; do
        [[ -d $MSYS2/share/licenses/$p ]] || continue
        mkdir -p "$DEST/third-party/msys2-$p"
        cp -r "$MSYS2/share/licenses/$p/." "$DEST/third-party/msys2-$p/"
    done
elif [[ -f /usr/share/doc/libeigen3-dev/copyright ]]; then
    put eigen /usr/share/doc/libeigen3-dev/copyright
fi

# --- SOURCE.txt ---------------------------------------------------------------
rev() {   # rev DIR - the commit a checkout is at, "+ local changes" when dirty
    local r
    r=$(git -C "$1" rev-parse HEAD 2>/dev/null) || { echo unknown; return; }
    if [[ -n $(git -C "$1" status --porcelain --untracked-files=no 2>/dev/null) ]]; then
        r="$r (with local changes)"
    fi
    echo "$r"
}
SUITE_REV=$(rev "$ROOT")
RTLSDR_REV=$(rev "$RTLSDR_DIR")
UWS_REV=$(rev "$UWS_DIR")
if [[ -n $MSYS2 ]] && command -v pacman >/dev/null; then
    v=$(pacman -Qo "$MSYS2/lib/libusb-1.0.a" 2>/dev/null | awk '{print $NF}') || v=
    LIBUSB="libusb ${v:-?} as packaged by MSYS2 (linked statically):
                                  https://github.com/msys2/MINGW-packages (mingw-w64-libusb),
                                  upstream https://github.com/libusb/libusb"
elif command -v dpkg-query >/dev/null; then
    v=$(dpkg-query -W -f '${Version}' libusb-1.0-0 2>/dev/null) || v=
    LIBUSB="libusb Debian package libusb-1.0-0 ${v:-?} (linked dynamically):
                                  apt-get source libusb-1.0"
else
    LIBUSB="libusb (the system's): https://github.com/libusb/libusb"
fi

cat > "$DEST/SOURCE.txt" <<EOF
Corresponding source of the GPL programs in this distribution
=============================================================

heimdall (heimdall_v2/) is free software under the GNU General Public License
version 3 or later (heimdall_v2/LICENSE). It contains the KrakenSDR librtlsdr
fork (GPL-2.0-or-later) and libusb (LGPL-2.1-or-later). rtl_test and
rtl_eeprom, where included, are the librtlsdr fork's tools (GPL-2.0-or-later,
third-party/librtlsdr/COPYING). You may copy, change and redistribute them
under those licences. Their complete source code:

  heimdall, the build scripts     https://github.com/krakenrf/krakensdr_suite
                                  commit $SUITE_REV
  KrakenSDR librtlsdr fork        https://github.com/krakenrf/librtlsdr
                                  commit $RTLSDR_REV
  uWebSockets / uSockets          https://github.com/uNetworking/uWebSockets
                                  commit $UWS_REV
  $LIBUSB
EOF
if [[ -n $GPL_PLUGINS ]]; then
    GPL_PLUGINS=${GPL_PLUGINS%$'\n'}
    cat >> "$DEST/SOURCE.txt" <<EOF

The decoder plugins below are programs of their own under the GNU GPL
(kraken_doa_v2/plugins/<id>/LICENSE and their files' headers). kraken_doa runs
each as a separate process; a plugin contains only its own code, the plugin
SDK (MIT, kraken_doa_v2/plugins/sdk/LICENSE), pffft and liquid-dsp. Their
complete source code:

$GPL_PLUGINS
EOF
fi
cat >> "$DEST/SOURCE.txt" <<EOF

If you cannot get it there, ask KrakenRF Inc.: krakenrf@krakensdr.com.

The rest of the KrakenSDR DoA software (kraken_doa_v2/) is not under the GPL:
see kraken_doa_v2/LICENSE (free for personal and hobbyist use and for a 180-day
evaluation; deployment and other commercial use need a licence from KrakenRF
Inc., with kraken_doa_v2/COMMERCIAL_TERMS.md).
EOF

echo "Licences collected in $DEST"
