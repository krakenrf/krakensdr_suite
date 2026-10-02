# syntax=docker/dockerfile:1
#
# KrakenSDR suite (heimdall + kraken_doa) as one image; docker-compose.yml runs
# it as two containers. See DOCKER.md.
#
# Build ON the device that runs it: kraken_doa compiles with -march=native, so
# an image built on a Pi 5 can die with "Illegal instruction" on a Pi 4.
#
#   docker compose build             (or: docker build -t krakensdr:local .)
#   docker compose build --build-arg JOBS=2   # 4 GB Pi; JOBS=1 on 2 GB

ARG DEBIAN_RELEASE=trixie

# ---------------------------------------------------------------- build ----
FROM debian:${DEBIAN_RELEASE}-slim AS build

# Never more than 3: -j4 on a Pi takes every core and the Eigen-heavy
# files need ~1 GB each (see kraken_doa_v2/CLAUDE.md)
ARG JOBS=3
# Pinned so a rebuild compiles what was tested (the Makefiles would clone
# uWebSockets master; its submodule pins uSockets)
ARG UWS_REPO=https://github.com/uNetworking/uWebSockets
ARG UWS_COMMIT=fe7da4cb05622b8d004718ec3ca05101782eb1c2
ARG LIBRTLSDR_REPO=https://github.com/krakenrf/librtlsdr
ARG LIBRTLSDR_COMMIT=08fb08165ecfcdd954c7a20cb1bbfbc159294f0e

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config ca-certificates \
        libusb-1.0-0-dev libfftw3-dev libeigen3-dev libssl-dev \
        libliquid-dev libopus-dev zlib1g-dev libsqlite3-dev \
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

COPY heimdall_v2/ heimdall_v2/
COPY kraken_doa_v2/ kraken_doa_v2/
RUN ln -s /src/uws-heimdall heimdall_v2/uWebSockets \
    && ln -s /src/uws-kraken kraken_doa_v2/uWebSockets \
    && make -C heimdall_v2 -j"$JOBS" \
    && make -C kraken_doa_v2 -j"$JOBS"

# Runtime packages = whatever owns the shared libraries the binaries load, so
# the runtime stage follows the Debian release without a hand-kept list
RUN for b in heimdall_v2/heimdall kraken_doa_v2/kraken_doa librtlsdr/build/src/rtl_test; do ldd "$b"; done \
        | awk '/=> \// {print $3}' | sort -u | xargs realpath | xargs dpkg -S \
        | cut -d: -f1 | sort -u > /src/runtime-packages \
    && cat /src/runtime-packages

# -------------------------------------------------------------- runtime ----
FROM debian:${DEBIAN_RELEASE}-slim

COPY --from=build /src/runtime-packages /tmp/runtime-packages
# openssl: TLS certificate for the client web UI on first start
# usbutils: lsusb for checking the passthrough
RUN apt-get update && apt-get install -y --no-install-recommends \
        $(cat /tmp/runtime-packages) openssl usbutils ca-certificates \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages

WORKDIR /opt/krakensdr
COPY --from=build /src/heimdall_v2/heimdall /src/heimdall_v2/index.html heimdall_v2/
COPY --from=build /src/kraken_doa_v2/kraken_doa \
                  /src/kraken_doa_v2/kraken_doa.html \
                  /src/kraken_doa_v2/array_calculator.html \
                  /src/kraken_doa_v2/opus-decoder.js \
                  /src/kraken_doa_v2/opus-decoder.min.js kraken_doa_v2/
COPY --from=build /src/librtlsdr/build/src/rtl_test /src/librtlsdr/build/src/rtl_eeprom /usr/local/bin/
COPY docker/entrypoint.sh /usr/local/bin/krakensdr-entrypoint
RUN chmod 755 /usr/local/bin/krakensdr-entrypoint

# State (settings, TLS cert, FFTW wisdom, DoA recordings) lives here
VOLUME /data
ENTRYPOINT ["/usr/local/bin/krakensdr-entrypoint"]
CMD ["help"]
