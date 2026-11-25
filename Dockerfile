# syntax=docker/dockerfile:1
#
# KrakenSDR suite (heimdall + kraken_doa) as one image; docker-compose.yml runs
# it as two containers. See DOCKER.md.
#
# Build ON the device that runs it: kraken_doa, heimdall and the plugins are
# compiled for the build CPU (-march=native / -mcpu=native, the Makefiles'
# flags), so an image built on a Pi 5 can die with "Illegal instruction" on a
# Pi 4, and a cross-build on a PC isn't supported.
#
#   docker compose build             (or: docker build -t krakensdr:local .)
#   docker compose build --build-arg JOBS=2   # 4 GB Pi; JOBS=1 on 2 GB

ARG DEBIAN_RELEASE=trixie

# ---------------------------------------------------------------- build ----
FROM debian:${DEBIAN_RELEASE}-slim AS build

# Never more than 3: -j4 on a Pi takes every core and the Eigen-heavy
# files need ~1 GB each (see kraken_doa_v2/CLAUDE.md)
ARG JOBS=3
# Pinned so a rebuild compiles what was tested - the same commit the
# Makefiles pin (UWS_COMMIT; keep them equal). Its submodule pins uSockets
ARG UWS_REPO=https://github.com/uNetworking/uWebSockets
ARG UWS_COMMIT=fe7da4cb05622b8d004718ec3ca05101782eb1c2
# librtlsdr: the fork commit with the newest feature heimdall uses
# (RTLSDR_FEATURE_MARK in heimdall_v2/Makefile: 9644d07, the fast tuner gain
# write; also the zero-copy buffers / FIFO hold and the tunes that drop no
# sample). A checkout of a commit has no upstream for the Makefile to
# fast-forward to: one before 578b43d (RTLSDR_FIX_MARK) stops the heimdall
# build, one before 9644d07 builds heimdall without the newer features
ARG LIBRTLSDR_REPO=https://github.com/krakenrf/librtlsdr
ARG LIBRTLSDR_COMMIT=9644d077cba347e7a57c84b6badc2c56ebe334d8

# libjpeg-dev (libjpeg-turbo, IJG/BSD-3-Clause/zlib): the decoder plugins link
# -ljpeg to compress video frames; the main app never links it. The runtime
# shared library (libjpeg-turbo8) needs no hand-adding - the runtime stage below
# derives its packages from ldd of every binary, which includes the plugin
# decoders.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config ca-certificates \
        libusb-1.0-0-dev libeigen3-dev libssl-dev \
        libopus-dev libjpeg-dev zlib1g-dev libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# KrakenSDR librtlsdr fork: static library for heimdall (same cmake options
# as heimdall's Makefile) plus rtl_test / rtl_eeprom for diagnosing the USB
# passthrough from inside the container
RUN git clone -q "$LIBRTLSDR_REPO" librtlsdr \
    && git -C librtlsdr checkout -q "$LIBRTLSDR_COMMIT" \
    && cmake -S librtlsdr -B librtlsdr/build -DCMAKE_BUILD_TYPE=Release \
        -DLINK_RTLTOOLS_AGAINST_STATIC_LIB=ON -DINSTALL_UDEV_RULES=OFF \
    && make -C librtlsdr/build -j"$JOBS" rtlsdr_static rtl_test rtl_eeprom

# uWebSockets, one copy per app, uSockets built the way each Makefile's
# install target builds it (heimdall without TLS, kraken_doa with OpenSSL)
RUN git init -q uws && cd uws \
    && git remote add origin "$UWS_REPO" \
    && git fetch -q --depth 1 origin "$UWS_COMMIT" && git checkout -q FETCH_HEAD \
    && git submodule update -q --init --depth 1 uSockets \
    && cd .. && cp -a uws uws-heimdall && mv uws uws-kraken \
    && make -C uws-heimdall/uSockets WITH_SSL=0 \
    && make -C uws-kraken/uSockets WITH_OPENSSL=1

# liquid-dsp (MIT) for kraken_doa + the plugins: built from source as a
# static library WITHOUT FFTW / libfec (Debian's libliquid1 links both). The
# pinned commit and the build are kraken_doa_v2/liquid.mk - the Makefiles'
# own recipe, run here in a layer of its own (rebuilt only when it changes)
COPY kraken_doa_v2/liquid.mk kraken_doa_v2/liquid.mk
RUN make -C kraken_doa_v2 -f liquid.mk -j"$JOBS" liquid

COPY heimdall_v2/ heimdall_v2/
COPY kraken_doa_v2/ kraken_doa_v2/
RUN ln -s /src/uws-heimdall heimdall_v2/uWebSockets \
    && ln -s /src/uws-kraken kraken_doa_v2/uWebSockets \
    && make -C heimdall_v2 -j"$JOBS" \
    && make -C kraken_doa_v2 -j"$JOBS"

# 🗣 Voice to Text: the local speech-to-text helper (whisper.cpp at the commit
# kraken_doa_v2/transcribe/setup.sh pins; ~10 more minutes on a Pi 5). The
# speech models are NOT in the image - they go to the data volume
# (/data/whisper_models): the sidebar's "⬇ Get". --build-arg WHISPER=0 leaves
# the helper out (the sidebar box then says it isn't installed)
ARG WHISPER=1
RUN mkdir -p /src/v2t \
    && if [ "$WHISPER" = 1 ]; then \
         JOBS="$JOBS" kraken_doa_v2/transcribe/setup.sh --build-only \
         && cp kraken_doa_v2/transcribe/build/kraken_whisper /src/v2t/; \
       fi

# Licences -> the image's /usr/share/doc/krakensdr (scripts/collect-licenses.sh):
# the suite's own, the third-party notices, the licence files of what was
# built from source here (librtlsdr fork, uWebSockets, liquid-dsp, whisper.cpp
# ...) and SOURCE.txt (where the GPL programs' source is). The Debian runtime
# packages keep theirs in /usr/share/doc/<package>
COPY LICENSE LICENSE
COPY scripts/collect-licenses.sh scripts/collect-licenses.sh
RUN bash scripts/collect-licenses.sh /src/licenses rtlsdr=/src/librtlsdr uws=/src/uws-kraken

# The version the web UIs show (docs/dev/version.md): the image has no git at
# run time, so it is written here from the build context's git metadata into
# /src/VERSION (empty when the context has none - a source archive, or a git
# worktree whose .git is a file - and the apps then show "Version unknown").
# The pattern .gi[t] lets the copy succeed without a .git; its contents land
# in /src/gitmeta next to the script. After the builds above, so a new commit
# re-runs only this step and the runtime stage's VERSION copies.
# --ignore-missing: the context holds only part of the tree, absent files
# are not local changes
COPY scripts/version.sh .gi[t] /src/gitmeta/
RUN GIT_DIR=/src/gitmeta GIT_WORK_TREE=/src sh /src/gitmeta/version.sh -C /src --ignore-missing > /src/VERSION \
    && echo "version: $(tr '\n' ' ' < /src/VERSION)" && rm -rf /src/gitmeta

# Runtime packages = whatever owns the shared libraries the binaries load, so
# the runtime stage follows the Debian release without a hand-kept list
RUN for b in heimdall_v2/heimdall kraken_doa_v2/kraken_doa librtlsdr/build/src/rtl_test \
             kraken_doa_v2/plugins/*/build/decoder /src/v2t/*; do [ -e "$b" ] && ldd "$b"; done \
        | awk '/=> \// {print $3}' | sort -u | xargs realpath | xargs dpkg -S \
        | cut -d: -f1 | sort -u > /src/runtime-packages \
    && cat /src/runtime-packages

# -------------------------------------------------------------- runtime ----
FROM debian:${DEBIAN_RELEASE}-slim

COPY --from=build /src/runtime-packages /tmp/runtime-packages
# openssl: the client web UI's HTTPS certificate (scripts/make-cert.sh at
# every start); iproute2: its `ip`, for the LAN addresses in wide scope
# usbutils: lsusb for checking the passthrough
# curl: Voice to Text's model downloads (the sidebar's "⬇ Get")
RUN apt-get update && apt-get install -y --no-install-recommends \
        $(cat /tmp/runtime-packages) openssl iproute2 usbutils ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages

WORKDIR /opt/krakensdr
COPY --from=build /src/heimdall_v2/heimdall /src/heimdall_v2/index.html heimdall_v2/
# (sigclass.mode[l]: Signal ID's model - a pattern, so a tree without the
# file still builds; the feature then says it is unavailable)
COPY --from=build /src/kraken_doa_v2/kraken_doa \
                  /src/kraken_doa_v2/kraken_doa.html \
                  /src/kraken_doa_v2/array_calculator.html \
                  /src/kraken_doa_v2/opus-decoder.min.js \
                  /src/kraken_doa_v2/sigclass.mode[l] kraken_doa_v2/
# decoder plugins (built ones: plugins/<id>/build/decoder); kraken_doa finds
# them through KRAKEN_PLUGIN_DIR (entrypoint)
COPY --from=build /src/kraken_doa_v2/plugins/ kraken_doa_v2/plugins/
# Voice to Text helper (empty with WHISPER=0); KRAKEN_TRANSCRIBE_DIR (entrypoint)
COPY --from=build /src/v2t/ kraken_doa_v2/transcribe/build/
COPY --from=build /src/librtlsdr/build/src/rtl_test /src/librtlsdr/build/src/rtl_eeprom /usr/local/bin/
COPY --from=build /src/licenses/ /usr/share/doc/krakensdr/
# the version (VERSION, written above; linked into each app's run folder by
# the entrypoint)
COPY --from=build /src/VERSION heimdall_v2/
COPY --from=build /src/VERSION kraken_doa_v2/
COPY docker/entrypoint.sh /usr/local/bin/krakensdr-entrypoint
# the web UI's HTTPS certificate (entrypoint: ensure_cert)
COPY scripts/make-cert.sh scripts/
RUN chmod 755 /usr/local/bin/krakensdr-entrypoint scripts/make-cert.sh

# State (settings, TLS cert, DoA / IQ recordings, ...) lives here
VOLUME /data
ENTRYPOINT ["/usr/local/bin/krakensdr-entrypoint"]
CMD ["help"]
