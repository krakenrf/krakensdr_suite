#!/bin/bash
# Voice to Text for kraken_doa (sidebar "🗣 Voice to Text"): builds the local,
# free speech-to-text helper (whisper.cpp, MIT licence) and downloads speech
# models. Nothing is sent anywhere: the models are downloaded once from
# Hugging Face (huggingface.co/ggerganov/whisper.cpp) and run on this machine.
#
#   transcribe/setup.sh                    build + base.en (default model) + the VAD model
#   transcribe/setup.sh small.en tiny.en   build + these models (+ the VAD model)
#   transcribe/setup.sh --models-only M..  only download models
#   transcribe/setup.sh --build-only       only build the helper
#   transcribe/setup.sh --update           rebuild if set up already (kraken_doa's make)
#   transcribe/setup.sh --list             the models that can be downloaded
#
# Environment: WHISPER_GPU = auto (default: the GPU through Metal on Apple
# Silicon, the CPU everywhere else) | on (Metal on any Mac - refused off
# macOS: no GPU backend is tested there) | off (CPU only); remembered for
# later builds (transcribe/build/.gpu_mode) until given again.
# JOBS (parallel compile jobs, default 3 - never more on a Pi),
# WHISPER_CMAKE_ARGS (extra CMake options, e.g. -DGGML_NATIVE=OFF for an image
# that must run on other CPUs), KRAKEN_WHISPER_MODELS (model folder, default
# transcribe/models).
# Windows: run it in the MSYS2 UCRT64 shell (or `make whisper` there). It
# builds build/kraken_whisper.exe for the CPU level of WIN_MARCH (default
# x86-64-v3 = AVX2 / FMA, like the rest of the suite; -march=native = this PC
# only); JOBS defaults to one per core and GB of free memory.
# macOS: the same script (Xcode command line tools + brew install cmake);
# Accelerate, and Metal on Apple Silicon (WHISPER_GPU above).
# After it: tick "Transcribe" on a VFO card in kraken_doa (no restart needed).
set -euo pipefail

SELF=$(readlink -f "$0")
# Windows: MSYS2's UCRT64 shell (uname MINGW64_NT-...) builds a native
# kraken_whisper.exe; the MSYS / Cygwin shells would build a program that
# needs their POSIX DLL
WINDOWS=0
MACOS=0
EXE=
case "$(uname -s)" in
    MINGW*) WINDOWS=1; EXE=.exe ;;
    Darwin) MACOS=1 ;;
    MSYS*|CYGWIN*)
        echo "setup.sh: run this in the MSYS2 UCRT64 shell, not the MSYS / Cygwin one (the helper must be a native Windows program)" >&2
        exit 1 ;;
esac
# a relative KRAKEN_WHISPER_MODELS is taken from where this is run (from
# kraken_doa_v2, as kraken_doa takes it) - before the cd below
if [ -n "${KRAKEN_WHISPER_MODELS:-}" ]; then
    # (Windows: C:\models = C:/models, which is absolute too)
    [ $WINDOWS = 1 ] && KRAKEN_WHISPER_MODELS=${KRAKEN_WHISPER_MODELS//\\//}
    case "$KRAKEN_WHISPER_MODELS" in
        /*) ;;
        [A-Za-z]:/*) [ $WINDOWS = 1 ] || KRAKEN_WHISPER_MODELS="$PWD/$KRAKEN_WHISPER_MODELS" ;;
        *) KRAKEN_WHISPER_MODELS="$PWD/$KRAKEN_WHISPER_MODELS" ;;
    esac
fi
cd "$(dirname "$SELF")"
# whisper.cpp is PINNED (a tested release): keep WHISPER_COMMIT = the tag's commit
WHISPER_TAG=v1.9.4
WHISPER_COMMIT=927cfce34f31707e17f2bff35c349632fb9e2c3a
WHISPER_REPO=https://github.com/ggml-org/whisper.cpp
MODEL_URL=https://huggingface.co/ggerganov/whisper.cpp/resolve/main
VAD_MODEL=silero-v6.2.0
VAD_URL=https://huggingface.co/ggml-org/whisper-vad/resolve/main
MODELS_DIR=${KRAKEN_WHISPER_MODELS:-models}
if [ $WINDOWS = 1 ]; then
    # a PC: one job per core, but no more than GB of free memory (the big
    # ggml / whisper files take ~0.5-1 GB each to compile)
    cores=$(nproc 2>/dev/null || echo 2)
    mem_gb=$(awk '/^MemAvailable:/ {print int($2 / 1048576)}' /proc/meminfo 2>/dev/null || echo 0)
    [ "${mem_gb:-0}" -ge 1 ] 2>/dev/null && [ "$mem_gb" -lt "$cores" ] && cores=$mem_gb
    JOBS=${JOBS:-$cores}
else
    JOBS=${JOBS:-3}
    [ "$JOBS" -gt 3 ] 2>/dev/null && JOBS=3
fi

# name  size_MB  note - the same list as MODEL_CATALOGUE in src/voice2text.cpp
CATALOGUE="tiny.en 75 fastest,_least_accurate
base.en 142 default:_good_on_a_Pi_5,_faster_than_real_time
base.en-q5_1 57 base.en_compressed:_less_memory,_about_as_accurate
small.en 466 most_accurate_on_a_Pi:_about_2x_slower_than_real_time
small.en-q5_1 181 small.en_compressed
tiny 75 any_language_(or_translated_to_English),_fastest
base 142 any_language_(or_translated_to_English)
base-q5_1 57 base_compressed
small 466 any_language,_most_accurate_on_a_Pi
small-q5_1 181 small_compressed
medium.en-q5_0 514 PC-class_CPU_only
large-v3-turbo-q5_0 547 PC-class_CPU_only,_best_for_other_languages"

die() { echo "setup.sh: $*" >&2; exit 1; }

list_models() {
    echo "Speech models (transcribe/setup.sh <name>):"
    echo "$CATALOGUE" | while read -r n s note; do
        printf "  %-20s %5s MB  %s\n" "$n" "$s" "${note//_/ }"
    done
}

known_model() { echo "$CATALOGUE" | awk '{print $1}' | grep -qx -- "$1"; }

fetch() {   # fetch <url> <file>: atomic, resumable-safe
    local url=$1 out=$2
    [ -s "$out" ] && { echo "  have $out"; return 0; }
    local curl=(curl)
    if [ $WINDOWS = 1 ]; then
        # curl.exe: MSYS2's, or Windows' own (System32, Windows 10 1803+),
        # whose Schannel fails the certificate revocation check without access
        # to the CA's OCSP / CRL servers (proxies) - best effort there
        command -v curl.exe >/dev/null ||
            die "curl.exe is needed to download models (Windows 10 1803+ has it; or pacman -S mingw-w64-ucrt-x86_64-curl)"
        curl=(curl.exe --ssl-revoke-best-effort)
    else
        command -v curl >/dev/null || die "curl is needed to download models (sudo apt install curl)"
    fi
    echo "  downloading $(basename "$out") ..."
    mkdir -p "$(dirname "$out")"
    "${curl[@]}" -fL --retry 3 --progress-bar -o "$out.part" "$url" || { rm -f "$out.part"; die "download failed: $url"; }
    # wc -c, not stat -c %s: the same on GNU and BSD (macOS)
    [ "$(wc -c < "$out.part" | tr -d ' ')" -gt 100000 ] || { rm -f "$out.part"; die "download too small: $url"; }
    mv "$out.part" "$out"
}

# Windows: the CMake options of the build. Ninja (no make in a native
# toolchain), no OpenMP (libgomp would be one more DLL - ggml's own threads
# instead), and the instruction set of the suite's WIN_MARCH in place of
# GGML_NATIVE's -march=native (AVX-512 on the build PC: an .exe that dies
# with "illegal instruction" on others). -muse-unaligned-vector-move: GCC
# doesn't keep the Win64 stack 32-byte aligned for AVX spills (GCC PR54412),
# and unaligned moves can't fault.
win_cmake_args() {
    local v3=(-DGGML_NATIVE=OFF -DGGML_SSE42=ON -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON)
    local isa
    case "${WIN_MARCH:--march=x86-64-v3}" in
        *native*) isa=(-DGGML_NATIVE=ON) ;;
        *x86-64-v4*) isa=("${v3[@]}" -DGGML_AVX512=ON) ;;
        *x86-64-v3*) isa=("${v3[@]}") ;;
        *x86-64-v2*) isa=(-DGGML_NATIVE=OFF -DGGML_SSE42=ON -DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_BMI2=OFF) ;;
        *x86-64*) isa=(-DGGML_NATIVE=OFF -DGGML_SSE42=OFF -DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_BMI2=OFF) ;;
        *) isa=("${v3[@]}") ;;
    esac
    PLATFORM_ARGS=(-G Ninja -DGGML_OPENMP=OFF "${isa[@]}"
              -DCMAKE_C_FLAGS=-Wa,-muse-unaligned-vector-move -DCMAKE_CXX_FLAGS=-Wa,-muse-unaligned-vector-move)
}

# WHISPER_GPU -> GPU_ARGS (CMake options) and GPU_MODE (what is remembered).
# Unset: the mode the last build used (else auto), so kraken_doa's make
# (--update) keeps a choice made with WHISPER_GPU=off / on.
# KRAKEN_WHISPER_GPU (transcribe/CMakeLists.txt) lets kraken_whisper use the
# GPU backend compiled in; GGML_METAL compiles Metal in (or leaves it out)
resolve_gpu() {
    GPU_MODE=${WHISPER_GPU:-$(cat build/.gpu_mode 2>/dev/null || echo auto)}
    local use
    case "$GPU_MODE" in
        auto) if [ $MACOS = 1 ] && [ "$(uname -m)" = arm64 ]; then use=1; else use=0; fi ;;
        on)   [ $MACOS = 1 ] || die "WHISPER_GPU=on: GPU builds are supported on macOS (Metal) only - leave it unset or off here"
              use=1 ;;
        off)  use=0 ;;
        *)    die "WHISPER_GPU must be auto, on or off (not '$GPU_MODE')" ;;
    esac
    GPU_NAME=CPU
    if [ $use = 1 ]; then
        # EMBED_LIBRARY explicitly: its default is GGML_METAL's at the FIRST
        # configure - a build/ first made with Metal off kept it off, and the
        # helper then looked for kernels/*.metal next to itself (model load
        # failed)
        GPU_ARGS=(-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DKRAKEN_WHISPER_GPU=ON)
        GPU_NAME="GPU (Metal)"
    elif [ $MACOS = 1 ]; then
        GPU_ARGS=(-DGGML_METAL=OFF -DGGML_METAL_EMBED_LIBRARY=OFF -DKRAKEN_WHISPER_GPU=OFF)
    else
        GPU_ARGS=(-DKRAKEN_WHISPER_GPU=OFF)
    fi
}

build() {
    resolve_gpu
    # Windows: core.longpaths (the checkout's deepest paths pass 260
    # characters under a long folder), core.autocrlf off (Git for Windows
    # would check the scripts out with CRLF)
    local git=(git)
    if [ $WINDOWS = 1 ]; then
        for t in git cmake g++ ninja windres; do
            command -v $t >/dev/null ||
                die "$t is missing (in the UCRT64 shell: pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja)"
        done
        case "$(g++ -dumpmachine)" in
            *-mingw32|*-windows-gnu) ;;
            *) die "g++ builds for $(g++ -dumpmachine), not native Windows - run this in the MSYS2 UCRT64 shell" ;;
        esac
        git=(git -c core.longpaths=true -c core.autocrlf=false)
        # CMake takes the processor from PROCESSOR_ARCHITECTURE, which a
        # stripped environment (env -i, some CI / service shells) lacks: ggml
        # then quietly builds its generic C code, several times slower
        if [ -z "${PROCESSOR_ARCHITECTURE:-}" ] && [ "$(uname -m)" = x86_64 ]; then
            export PROCESSOR_ARCHITECTURE=AMD64
        fi
    elif [ $MACOS = 1 ]; then
        for t in git cmake c++ make; do
            command -v $t >/dev/null || die "$t is missing (xcode-select --install; brew install cmake)"
        done
    else
        for t in git cmake g++ make; do command -v $t >/dev/null || die "$t is missing (sudo apt install git cmake build-essential)"; done
    fi
    if [ ! -d whisper.cpp/.git ]; then
        echo "Cloning whisper.cpp $WHISPER_TAG ..."
        rm -rf whisper.cpp
        "${git[@]}" clone --depth 1 --branch "$WHISPER_TAG" "$WHISPER_REPO" whisper.cpp
    fi
    if [ "$(git -C whisper.cpp rev-parse HEAD)" != "$WHISPER_COMMIT" ]; then
        echo "Switching whisper.cpp to the pinned $WHISPER_TAG ..."
        "${git[@]}" -C whisper.cpp fetch --depth 1 origin tag "$WHISPER_TAG"
        "${git[@]}" -C whisper.cpp checkout -q "$WHISPER_TAG"
        rm -rf build
    fi
    [ "$(git -C whisper.cpp rev-parse HEAD)" = "$WHISPER_COMMIT" ] || die "whisper.cpp is not at $WHISPER_COMMIT"
    # incremental only while the GPU choice is the one the build was configured with
    if [ "$quiet" = 1 ] && [ -f build/CMakeCache.txt ] && [ "$(cat build/.gpu_args 2>/dev/null)" = "${GPU_ARGS[*]}" ]; then
        # kraken_doa's make: incremental, silent unless something is rebuilt
        out=$(cmake --build build --target kraken_whisper -j "$JOBS" 2>&1) || { echo "$out"; exit 1; }
        echo "$out" | grep -q "Linking" && echo "Rebuilt transcribe/build/kraken_whisper$EXE"
        return 0
    fi
    # Intel Macs: their integrated GPUs are often slower than the CPU path
    # (Accelerate - measured on a MacBook Air 2015: 4.6 s Metal, 2.6 s CPU
    # for jfk.wav), old GPU drivers add risk: Metal there only when asked for
    if [ $MACOS = 1 ] && [ "$(uname -m)" = x86_64 ] && [ "$GPU_NAME" != CPU ]; then
        echo "WHISPER_GPU=on on an Intel Mac: Metal may well be slower than the CPU here"
    fi
    PLATFORM_ARGS=()
    if [ $WINDOWS = 1 ]; then
        win_cmake_args
        # a build/ configured for another generator (an earlier try) can't be reused
        if [ -f build/CMakeCache.txt ] && ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' build/CMakeCache.txt; then
            rm -rf build
        fi
        echo "Building kraken_whisper.exe ($JOBS jobs, ${WIN_MARCH:--march=x86-64-v3} - a few minutes on a PC) ..."
    else
        echo "Building kraken_whisper ($JOBS jobs, $GPU_NAME - takes ~10 min on a Pi 5) ..."
    fi
    # shellcheck disable=SC2086
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ${PLATFORM_ARGS[@]+"${PLATFORM_ARGS[@]}"} "${GPU_ARGS[@]}" ${WHISPER_CMAKE_ARGS:-} >/dev/null
    cmake --build build --target kraken_whisper -j "$JOBS"
    echo "$GPU_MODE" > build/.gpu_mode
    echo "${GPU_ARGS[*]}" > build/.gpu_args
    echo "Built transcribe/build/kraken_whisper$EXE"
}

do_build=1
do_models=1
quiet=0
models=()
for a in "$@"; do
    case "$a" in
        --list) list_models; exit 0 ;;
        --models-only) do_build=0 ;;
        --build-only) do_models=0 ;;
        --update) [ -x build/kraken_whisper$EXE ] || exit 0; do_models=0; quiet=1 ;;
        -h|--help) sed -n '2,28p' "$SELF" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option $a (see --help)" ;;
        *) known_model "$a" || { list_models; die "unknown model $a"; }; models+=("$a") ;;
    esac
done

[ $do_build = 1 ] && build
if [ $do_models = 1 ]; then
    if [ ${#models[@]} -eq 0 ] && ! ls "$MODELS_DIR"/ggml-*.bin 2>/dev/null | grep -qv silero; then
        models=(base.en)
    fi
    echo "Models -> $MODELS_DIR"
    # (${a[@]+...}: bash 3.2 - macOS's - calls an empty array unbound under set -u)
    for m in ${models[@]+"${models[@]}"}; do fetch "$MODEL_URL/ggml-$m.bin" "$MODELS_DIR/ggml-$m.bin"; done
    # voice activity detection: skips noise / silence before whisper (which
    # otherwise "hears" words in noise)
    fetch "$VAD_URL/ggml-$VAD_MODEL.bin" "$MODELS_DIR/ggml-$VAD_MODEL.bin"
fi
[ "$quiet" = 1 ] || echo "Done. In kraken_doa: tick \"Transcribe\" on a VFO card; transcripts appear in the 🗣 Voice tab."
