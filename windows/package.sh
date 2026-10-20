#!/usr/bin/env bash
#
# windows/package.sh - copy what the Windows build needs at run time into
# dist/KrakenSDR-windows/ and zip it: a folder that runs on any Windows 10 / 11
# x64 PC without MSYS2 (every exe is static). Run after windows/build.sh, in
# the MSYS2 UCRT64 shell:
#
#   windows/package.sh [output folder]     (default dist/KrakenSDR-windows)
#
# The layout is the repository's (heimdall_v2\, kraken_doa_v2\, plugins under
# it): every runtime file path of the apps is relative to their folder (as in
# the Docker image, whose COPY list this follows). Settings, recordings,
# certificates and keys of THIS machine are not copied - the package starts
# fresh (the client creates its own TLS certificate on the first start).
set -Eeuo pipefail
trap 'echo -e "\n\033[31m\033[1mpackage.sh ABORTED\033[0m at line $LINENO: $BASH_COMMAND" >&2' ERR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/dist/KrakenSDR-windows}"
K="$ROOT/kraken_doa_v2"

need() { [[ -f "$1" ]] || { echo "missing $1 - run windows/build.sh first" >&2; exit 1; }; }
need "$ROOT/krakensdr.exe"
need "$ROOT/heimdall_v2/heimdall.exe"
need "$K/kraken_doa.exe"

rm -rf "$OUT"
mkdir -p "$OUT/heimdall_v2" "$OUT/kraken_doa_v2/plugins" "$OUT/tools"

# launcher + docs
cp "$ROOT/krakensdr.exe" "$ROOT/run.bat" "$OUT/"
[[ -f "$ROOT/WINDOWS.md" ]] && cp "$ROOT/WINDOWS.md" "$OUT/"

# heimdall: the exe + its web UI
cp "$ROOT/heimdall_v2/heimdall.exe" "$ROOT/heimdall_v2/index.html" "$OUT/heimdall_v2/"

# kraken_doa: the exe + the files it serves (Dockerfile COPY list)
cp "$K/kraken_doa.exe" "$K/kraken_doa.html" "$K/array_calculator.html" \
   "$K/opus-decoder.js" "$K/opus-decoder.min.js" "$OUT/kraken_doa_v2/"

# decoder plugins: the built exe of each, their test-signal catalogues and
# generators (Debug Playback; they need Python), the codec check, the SDK tools
n=0
for exe in "$K"/plugins/*/build/decoder.exe; do
    [[ -f "$exe" ]] || continue
    id="$(basename "$(dirname "$(dirname "$exe")")")"
    mkdir -p "$OUT/kraken_doa_v2/plugins/$id/build"
    cp "$exe" "$OUT/kraken_doa_v2/plugins/$id/build/"
    [[ -d "$K/plugins/$id/tools" ]] && cp -r "$K/plugins/$id/tools" "$OUT/kraken_doa_v2/plugins/$id/"
    n=$((n + 1))
done
if [[ -f "$K/plugins/lib/build/codecs.exe" ]]; then
    mkdir -p "$OUT/kraken_doa_v2/plugins/lib/build"
    cp "$K/plugins/lib/build/codecs.exe" "$OUT/kraken_doa_v2/plugins/lib/build/"
fi
[[ -d "$K/plugins/sdk/tools" ]] && mkdir -p "$OUT/kraken_doa_v2/plugins/sdk" && cp -r "$K/plugins/sdk/tools" "$OUT/kraken_doa_v2/plugins/sdk/"

# AI Signal Lab bridge (Python; off until set up) and Voice to Text, when built
if [[ -d "$K/ai" ]]; then
    mkdir -p "$OUT/kraken_doa_v2/ai"
    find "$K/ai" -maxdepth 2 -name '*.py' | while read -r f; do
        rel="${f#$K/ai/}"
        mkdir -p "$OUT/kraken_doa_v2/ai/$(dirname "$rel")"
        cp "$f" "$OUT/kraken_doa_v2/ai/$rel"
    done
fi
if [[ -f "$K/transcribe/build/kraken_whisper.exe" ]]; then
    mkdir -p "$OUT/kraken_doa_v2/transcribe/build"
    cp "$K/transcribe/build/kraken_whisper.exe" "$OUT/kraken_doa_v2/transcribe/build/"
fi

# the fork's dongle check tools (static; one linked before heimdall's make
# gave them the static libusb needs libusb's DLL next to it)
for t in rtl_test rtl_eeprom; do
    exe="$ROOT/librtlsdr/build/src/$t.exe"
    [[ -f "$exe" ]] || continue
    cp "$exe" "$OUT/tools/"
    if objdump -p "$exe" | grep -i 'DLL Name: libusb-1.0.dll' >/dev/null; then
        cp "${MINGW_PREFIX:-/ucrt64}/bin/libusb-1.0.dll" "$OUT/tools/"
    fi
done

echo "Packaged $OUT  (heimdall, kraken_doa, $n decoder plugins)"

# zip next to the folder (PowerShell's Compress-Archive: no extra package needed)
ZIP="$OUT.zip"
rm -f "$ZIP"
powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$OUT")\\*' -DestinationPath '$(cygpath -w "$ZIP")' -Force" &&
    echo "Zipped  $ZIP ($(du -h "$ZIP" | cut -f1))"
