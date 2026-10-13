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
# Environment: JOBS (parallel compile jobs, default 3 - never more on a Pi),
# WHISPER_CMAKE_ARGS (extra CMake options, e.g. -DGGML_NATIVE=OFF for an image
# that must run on other CPUs), KRAKEN_WHISPER_MODELS (model folder, default
# transcribe/models).
# After it: tick "Transcribe" on a VFO card in kraken_doa (no restart needed).
set -euo pipefail

SELF=$(readlink -f "$0")
# a relative KRAKEN_WHISPER_MODELS is taken from where this is run (from
# kraken_doa_v2, as kraken_doa takes it) - before the cd below
if [ -n "${KRAKEN_WHISPER_MODELS:-}" ]; then
    case "$KRAKEN_WHISPER_MODELS" in /*) ;; *) KRAKEN_WHISPER_MODELS="$PWD/$KRAKEN_WHISPER_MODELS" ;; esac
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
JOBS=${JOBS:-3}
[ "$JOBS" -gt 3 ] 2>/dev/null && JOBS=3

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
    command -v curl >/dev/null || die "curl is needed to download models (sudo apt install curl)"
    echo "  downloading $(basename "$out") ..."
    mkdir -p "$(dirname "$out")"
    curl -fL --retry 3 --progress-bar -o "$out.part" "$url" || { rm -f "$out.part"; die "download failed: $url"; }
    [ "$(stat -c %s "$out.part")" -gt 100000 ] || { rm -f "$out.part"; die "download too small: $url"; }
    mv "$out.part" "$out"
}

build() {
    for t in git cmake g++ make; do command -v $t >/dev/null || die "$t is missing (sudo apt install git cmake build-essential)"; done
    if [ ! -d whisper.cpp/.git ]; then
        echo "Cloning whisper.cpp $WHISPER_TAG ..."
        rm -rf whisper.cpp
        git clone --depth 1 --branch "$WHISPER_TAG" "$WHISPER_REPO" whisper.cpp
    fi
    if [ "$(git -C whisper.cpp rev-parse HEAD)" != "$WHISPER_COMMIT" ]; then
        echo "Switching whisper.cpp to the pinned $WHISPER_TAG ..."
        git -C whisper.cpp fetch --depth 1 origin tag "$WHISPER_TAG"
        git -C whisper.cpp checkout -q "$WHISPER_TAG"
        rm -rf build
    fi
    [ "$(git -C whisper.cpp rev-parse HEAD)" = "$WHISPER_COMMIT" ] || die "whisper.cpp is not at $WHISPER_COMMIT"
    if [ "$quiet" = 1 ] && [ -f build/CMakeCache.txt ]; then
        # kraken_doa's make: incremental, silent unless something is rebuilt
        out=$(cmake --build build --target kraken_whisper -j "$JOBS" 2>&1) || { echo "$out"; exit 1; }
        echo "$out" | grep -q "Linking" && echo "Rebuilt transcribe/build/kraken_whisper"
        return 0
    fi
    echo "Building kraken_whisper ($JOBS jobs - takes ~10 min on a Pi 5) ..."
    # shellcheck disable=SC2086
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ${WHISPER_CMAKE_ARGS:-} >/dev/null
    cmake --build build --target kraken_whisper -j "$JOBS"
    echo "Built transcribe/build/kraken_whisper"
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
        --update) [ -x build/kraken_whisper ] || exit 0; do_models=0; quiet=1 ;;
        -h|--help) sed -n '2,20p' "$SELF" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
    for m in "${models[@]}"; do fetch "$MODEL_URL/ggml-$m.bin" "$MODELS_DIR/ggml-$m.bin"; done
    # voice activity detection: skips noise / silence before whisper (which
    # otherwise "hears" words in noise)
    fetch "$VAD_URL/ggml-$VAD_MODEL.bin" "$MODELS_DIR/ggml-$VAD_MODEL.bin"
fi
[ "$quiet" = 1 ] || echo "Done. In kraken_doa: tick \"Transcribe\" on a VFO card; transcripts appear in the 🗣 Voice tab."
