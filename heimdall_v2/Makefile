# Heimdall Standalone Makefile with uWebSockets and Eigen - Modular Version
# Enhanced with Eigenvalue Decomposition for Phase Calibration

CC = g++
TARGET = heimdall
CONFIG_HEADER = config.h

# Windows: a native build with MSYS2's UCRT64 toolchain (MinGW-w64 GCC; see
# ../WINDOWS.md) - run `make` in the UCRT64 shell, or ../build_windows.bat.
# Everything Windows-specific in this Makefile is under WINDOWS; the Linux
# build is unchanged.
WINDOWS := $(if $(findstring mingw,$(shell $(CC) -dumpmachine 2>/dev/null)),1,)
ifeq ($(WINDOWS),1)
TARGET = heimdall.exe
# linked to a temporary name, then moved: Windows can't overwrite a running
# exe, MSYS2's mv moves the busy one aside (a rebuild while the stack runs)
LINK_TMP = .tmp
endif

# macOS (Apple clang + Homebrew: eigen, openssl@3, libusb, cmake, pkg-config -
# `make deps-macos`): the Linux build plus the shims of mac/posix_compat.h
# (force-included) and Homebrew's keg-only OpenSSL. Everything macOS-specific
# is under MACOS.
MACOS := $(if $(filter Darwin,$(shell uname -s)),1,)
ifeq ($(MACOS),1)
# one deployment target for everything (this macOS version, as CMake picks
# it for librtlsdr / liquid-dsp): no "built for newer macOS" link warnings
export MACOSX_DEPLOYMENT_TARGET ?= $(shell sw_vers -productVersion | cut -d. -f1-2)
endif

# Source files organized by module
CORE_SOURCES = src/core/utils.cpp src/core/settings.cpp src/core/forward_comp.cpp src/core/status_dashboard.cpp \
               src/core/platform.cpp src/core/fs_compat.cpp
SDR_SOURCES = src/sdr/sdr_device.cpp src/sdr/sdr_init.cpp src/sdr/sdr_pipeline.cpp src/sdr/pipeline_control.cpp src/sdr/downconverter.cpp src/sdr/kerberos_gpio.cpp
DSP_SOURCES = src/dsp/fft_plan.cpp src/dsp/correlation.cpp src/dsp/compensation.cpp
NET_SOURCES = src/net/tcp_common.cpp src/net/tcp_data_server.cpp src/net/tcp_control_server.cpp src/net/rtl_tcp_server.cpp
WEB_SOURCES = src/web/html_loader.cpp src/web/web_server.cpp
MAIN_SOURCE = src/main.cpp

# All source files
SOURCES = $(CORE_SOURCES) $(SDR_SOURCES) $(DSP_SOURCES) $(NET_SOURCES) $(WEB_SOURCES) $(MAIN_SOURCE)

# Object files (in build directory)
BUILD_DIR = build
# the compiler flags the objects were built with (rule further down)
FLAGS_STAMP = $(BUILD_DIR)/.flags
# FFT: pffft, vendored in external/pffft (plain C, BSD-style licence; replaced
# FFTW 2026-10-17) - the correlation FFTs (src/dsp/fft_plan.cpp)
PFFFT_OBJECTS = $(BUILD_DIR)/pffft/pffft.o $(BUILD_DIR)/pffft/pffft_common.o
OBJECTS = $(SOURCES:src/%.cpp=$(BUILD_DIR)/%.o) $(PFFFT_OBJECTS)
# Windows: process setup (binary file mode, timers, Winsock - win/win_init.cpp)
# and the manifest (UTF-8 code page, long paths) in every exe
ifeq ($(WINDOWS),1)
WIN_OBJECTS = $(BUILD_DIR)/win/win_init.o $(BUILD_DIR)/win/app_res.o
OBJECTS += $(WIN_OBJECTS)
endif

# Check if uWebSockets is installed
UWS_EXISTS := $(shell test -d uWebSockets && echo yes || echo no)
# uWebSockets is pinned to the commit both apps are tested with (the
# Dockerfile pins the same one). Newer master adds HttpCache.h, which defines
# uWS::time_ms() in a header without `inline` - every object that includes
# App.h gets a copy and the link fails ("multiple definition of
# uWS::time_ms()"). An existing checkout on another commit (cloned from
# master before this pin) is switched here, before any rule runs, and the
# objects built against its headers are dropped. Override: make UWS_COMMIT=...
UWS_REPO ?= https://github.com/uNetworking/uWebSockets
UWS_COMMIT ?= fe7da4cb05622b8d004718ec3ca05101782eb1c2
UWS_HEAD := $(shell git -C uWebSockets rev-parse HEAD 2>/dev/null)
ifneq ($(UWS_HEAD),)
ifneq ($(UWS_HEAD),$(UWS_COMMIT))
$(info uWebSockets is at $(UWS_HEAD) - switching to the tested $(UWS_COMMIT))
UWS_SWITCH := $(shell (git -C uWebSockets checkout -q $(UWS_COMMIT) 2>/dev/null || \
    (git -C uWebSockets fetch -q origin $(UWS_COMMIT) && git -C uWebSockets checkout -q $(UWS_COMMIT))) && \
    git -C uWebSockets submodule update -q --init uSockets && \
    rm -f uWebSockets/uSockets/*.o && rm -rf $(BUILD_DIR)/core $(BUILD_DIR)/sdr $(BUILD_DIR)/dsp $(BUILD_DIR)/net $(BUILD_DIR)/web && echo ok)
ifneq ($(UWS_SWITCH),ok)
$(error could not switch uWebSockets to $(UWS_COMMIT) - delete the uWebSockets folder and run make again)
endif
endif
endif

# Check if Eigen is installed
EIGEN_EXISTS := $(shell pkg-config --exists eigen3 2>/dev/null && echo yes || echo no)

# KrakenSDR librtlsdr fork, linked STATICALLY. heimdall needs the fork, and a
# distro librtlsdr0 (pulled in by gqrx, gr-osmosdr, rtl_433, ...) sits earlier
# in the loader's search path than /usr/local/lib, so a dynamically linked
# heimdall would silently load the stock library instead. Linked in, the fork
# can't be shadowed, and the distro packages can stay installed. Built from
# ../librtlsdr (fetched by install.sh, or cloned here when missing).
RTLSDR_DIR  = ../librtlsdr
RTLSDR_REPO = https://github.com/krakenrf/librtlsdr
RTLSDR_LIB  = $(RTLSDR_DIR)/build/src/librtlsdr.a
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -lusb-1.0)
RTLSDR_CMAKE_FLAGS = -DCMAKE_BUILD_TYPE=Release -DLINK_RTLTOOLS_AGAINST_STATIC_LIB=ON -DINSTALL_UDEV_RULES=ON
# heimdall requires the fork at krakenrf/librtlsdr 578b43d or later, on every
# platform: the drag calls rtlsdr_set_center_freq_fast / rtlsdr_finish_tune
# (RTLSDR_TUNE_KEEP_AUTOTUNE; an older fork builds heimdall's full retune
# path only), the R820T retune in fewer USB transfers (unchanged registers
# skipped, one N / SDM write, the I2C repeater left on, a 5 ms PLL settle
# after a divider / band change), and a control OUT that stalls with
# LIBUSB_ERROR_PIPE retried after a dummy demod read (Linux too: a >= 300 ms
# pause inside a tuner access failed the next I2C write). Under _WIN32 only,
# since 00606c0 (bd357dd + its follow-up): cancelling a stream waits until
# every transfer's callback has finished before freeing it (rtlsdr_close
# crashed after a stop), and the bulk endpoint streams with WinUSB's RAW_IO
# policy (without it the dongles dropped 94-sample packets a few times a
# minute - tuners out of alignment), switched off again when the stream ends
# (a later rtlsdr_read_sync failed).
# RTLSDR_FIX_MARK = a word only that librtlsdr.c has (578b43d's
# rtlsdr_finish_tune; 00606c0 has none). A clone from before is
# fast-forwarded to its upstream (git fetch + merge --ff-only - what
# install.sh's `git pull --ff-only` does); local changes the merge would
# overwrite stop it and are never discarded (Windows only: a librtlsdr.c
# this Makefile once patched with win/librtlsdr-windows.patch, bd357dd's
# fixes - the patch is gone - is restored first). The working librtlsdr.c is
# checked on every make (RTLSDR_CHECK), so a library built from an older
# tree is never linked: while it lacks the mark, every make tries the update
# and stops with instructions if that fails (offline, detached HEAD - e.g. a
# checkout of an older commit -, no upstream, local changes in the way). A
# HEAD from before with the mark in the working file (applied by hand) is
# updated once per HEAD (RTLSDR_TRIED), not on every make; git unable to read
# the clone = no update. Linux: only this static library is rebuilt - the
# rtl_* tools install.sh put in /usr/local stay until install.sh runs again.
RTLSDR_FIX_MARK = rtlsdr_finish_tune
RTLSDR_TRIED = $(RTLSDR_DIR)/build/.fork_update_tried
RTLSDR_FILE_OK := $(shell grep -qs $(RTLSDR_FIX_MARK) $(RTLSDR_DIR)/src/librtlsdr.c && echo yes)
ifeq ($(RTLSDR_FILE_OK),)
RTLSDR_CHECK = rtlsdr-check
endif
ifneq ($(wildcard $(RTLSDR_DIR)/.git),)
RTLSDR_HEAD := $(shell git -C $(RTLSDR_DIR) rev-parse -q --verify HEAD 2>/dev/null)
ifneq ($(RTLSDR_HEAD),)
ifeq ($(shell git -C $(RTLSDR_DIR) grep -q $(RTLSDR_FIX_MARK) HEAD -- src/librtlsdr.c 2>/dev/null && echo yes),)
ifeq ($(RTLSDR_FILE_OK),)
RTLSDR_UPDATE = rtlsdr-update
else ifneq ($(RTLSDR_HEAD),$(shell cat $(RTLSDR_TRIED) 2>/dev/null))
RTLSDR_UPDATE = rtlsdr-update
endif
endif
endif
endif
# Windows: the fork's CMake names the static library librtlsdr_static.a there
# (librtlsdr.a would be its DLL's import library); libusb reaches the dongles
# through the WinUSB driver (installed per dongle with Zadig, ../WINDOWS.md).
# The fork's rtl_* tools need gnu17 + sys/time.h with GCC 15+ (C23 default).
ifeq ($(WINDOWS),1)
RTLSDR_LIB  = $(RTLSDR_DIR)/build/src/librtlsdr_static.a
RTLSDR_CMAKE_FLAGS = -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DLINK_RTLTOOLS_AGAINST_STATIC_LIB=ON \
    -DCMAKE_C_FLAGS="-std=gnu17 -include sys/time.h -Wno-error=incompatible-pointer-types" \
    -DCMAKE_EXE_LINKER_FLAGS=-static
# The rtl_* tools (rtl_test / rtl_eeprom, ../windows/build.sh) static too: the
# fork's FindLibUSB finds libusb's DLL import library and links it by its full
# path, which -static doesn't change - the tools needed MSYS2's
# libusb-1.0.dll. -ULIBUSB_FOUND: with LIBUSB_FOUND cached FindLibUSB skips
# everything, its -DHAVE_LIBUSB_* defines too, so a reconfigure of an existing
# build dir compiled the library without them (unlike a fresh one)
RTLSDR_CMAKE_FLAGS += -ULIBUSB_FOUND \
    -DLIBUSB_LIBRARIES:FILEPATH=$(shell pkg-config --variable=libdir libusb-1.0)/libusb-1.0.a
# a build dir configured with the import library (before the line above) is
# configured again
ifneq ($(shell grep -s '^LIBUSB_LIBRARIES:' $(RTLSDR_DIR)/build/CMakeCache.txt | grep 'dll\.a'),)
RTLSDR_RECONFIGURE = rtlsdr-reconfigure
endif
endif
# Every platform: the library is rebuilt when the fork's sources change (a
# pull, the update above), not only when it is missing - heimdall
# compiles against the fork's current headers, and a header with a new call
# (rtlsdr_set_center_freq_fast) next to an older library failed the link
RTLSDR_SRCS = $(wildcard $(RTLSDR_DIR)/src/*.c $(RTLSDR_DIR)/src/*.h $(RTLSDR_DIR)/include/*.h)

# C++20 required for uWebSockets
# -fno-math-errno: sqrt & co. without an errno path (loops with them
# vectorize); -fcx-fortran-rules: complex * and / without C99's inf / NaN
# recovery (a __mulsc3 / __divsc3 call that keeps a complex loop - the
# cross-spectrum - from vectorizing; division keeps its range scaling).
# Unlike -ffast-math neither touches NaN tests or the order of sums
CFLAGS = -Wall -Wextra -std=c++20 -D_GNU_SOURCE -DUWS_NO_ZLIB -fno-math-errno -fcx-fortran-rules
CFLAGS += -I. -Isrc -Isrc/core -Isrc/sdr -Isrc/dsp -Isrc/net -Isrc/web -Iexternal
CFLAGS += -I$(RTLSDR_DIR)/include   # the fork's headers, never a system rtl-sdr.h
LDFLAGS = $(RTLSDR_LIB) $(LIBUSB_LIBS) -lpthread -lm -lz

# Add Eigen include path
ifeq ($(EIGEN_EXISTS),yes)
    CFLAGS += $(shell pkg-config --cflags eigen3)
else
    # Fallback to common Eigen location
    CFLAGS += -I/usr/include/eigen3
endif

# Add SSL if available (optional for uWebSockets)
LDFLAGS += -lssl -lcrypto

# macOS: Apple clang has no -fcx-fortran-rules (GCC only) - mac/fast_complex.h
# takes libc++'s inf / NaN recovery out of std::complex * and norm instead;
# OpenSSL from Homebrew is keg-only (not in the default search paths)
ifeq ($(MACOS),1)
    CFLAGS := $(filter-out -fcx-fortran-rules,$(CFLAGS)) -include mac/posix_compat.h -include mac/fast_complex.h
    OPENSSL_PREFIX := $(or $(shell brew --prefix openssl@3 2>/dev/null),/usr/local/opt/openssl@3)
    CFLAGS += -I$(OPENSSL_PREFIX)/include
    LDFLAGS += -L$(OPENSSL_PREFIX)/lib -framework IOKit -framework CoreFoundation
endif

# Architecture detection
ARCH := $(shell uname -m)
# macOS on Apple Silicon says arm64 (Linux: aarch64) - the same 64-bit ARM
# with NEON. `override`: also when ARCH=arm64 is given on the command line
ifeq ($(ARCH),arm64)
override ARCH := aarch64
endif
# the aarch64 CPU flag: this CPU (-mcpu=native) - except on macOS, where
# older Apple clang rejects "native" on arm64 and its default target is
# already the M1 (NEON, dot product, fp16); `make MAC_CPU=-mcpu=apple-m2`
# for a newer chip's extras
MAC_CPU ?=
AARCH64_CPU = -mcpu=native
ifeq ($(MACOS),1)
AARCH64_CPU = $(MAC_CPU)
endif

# pffft is C (CC above is g++): built with gcc, its SIMD named explicitly on
# ARM - without -DPFFFT_ENABLE_NEON it silently builds its scalar code
PFFFT_CC = gcc
PFFFT_CFLAGS = -O3 -Wno-unused-parameter

ifeq ($(WINDOWS),1)
    # An exe that runs on other PCs too: AVX2 (x86-64-v3, Haswell / Zen and
    # later) rather than -march=native (this build machine's AVX-512 crashes
    # elsewhere with an illegal instruction); `make WIN_MARCH=-march=native`
    # for this PC only. -muse-unaligned-vector-move: GCC can't align the
    # stack to 32 bytes on Win64 (GCC bug 54412), so an AVX spill with an
    # aligned move could fault - the assembler emits unaligned moves instead
    # (no cost on these CPUs).
    WIN_MARCH ?= -march=x86-64-v3 -mtune=generic
    CFLAGS += -O3 $(WIN_MARCH) -Wa,-muse-unaligned-vector-move
    PFFFT_CFLAGS += $(WIN_MARCH) -Wa,-muse-unaligned-vector-move
    # Windows 10+ API, windows.h without min/max macros and GDI (its ERROR
    # macro), M_PI & co. under -std=c++20, 64-bit off_t / stat sizes, libuv
    # under uSockets, and the POSIX functions MinGW lacks (win/posix_compat.h,
    # force-included)
    # PFFFT_STATIC_DEFINE: pffft.h otherwise declares its API dllimport on _WIN32
    CFLAGS += -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNOGDI \
              -D_USE_MATH_DEFINES -D_FILE_OFFSET_BITS=64 -DLIBUS_USE_LIBUV -DPFFFT_STATIC_DEFINE \
              -include win/posix_compat.h
    PFFFT_CFLAGS += -DPFFFT_STATIC_DEFINE
else ifeq ($(ARCH),aarch64)
    # -mcpu=native: this CPU's instructions AND its tuning (a Pi 5: Cortex-A76,
    # LSE atomics inline; it was -march=armv8-a -mtune=cortex-a72, the Pi 4's
    # core). Like kraken_doa and the plugins, the build then runs only on the
    # kind of CPU it was built on
    CFLAGS += -O3 $(AARCH64_CPU)
    PFFFT_CFLAGS += $(AARCH64_CPU) -DPFFFT_ENABLE_NEON
else ifeq ($(ARCH),x86_64)
    CFLAGS += -O3 -march=native -mtune=native
    PFFFT_CFLAGS += -march=native
else ifeq ($(ARCH),armv7l)
    CFLAGS += -O2
    PFFFT_CFLAGS += -mfpu=neon -DPFFFT_ENABLE_NEON
else
    CFLAGS += -O2
endif

# Windows: one self-contained exe (no MSYS2 DLLs needed to run it) - static
# libstdc++ / winpthread / libusb / libuv (uSockets' event loop there: no
# epoll) / OpenSSL / zlib, plus the Windows system libraries they use
ifeq ($(WINDOWS),1)
    LDFLAGS = -static $(RTLSDR_LIB) $(shell pkg-config --static --libs libusb-1.0 libuv) \
              -lssl -lcrypto -lz -lws2_32 -lcrypt32 -lbcrypt -lgdi32 -luser32 -ladvapi32 \
              -lwinmm -lavrt -lsetupapi -lhid -lpthread
endif

# Default target
all: check-deps $(TARGET)

# Check dependencies
check-deps: check-uws check-eigen

# Check if uWebSockets is installed
# clones / switches to UWS_COMMIT (see above) and builds uSockets if needed
check-uws: install-uws

# Check if Eigen is installed
check-eigen:
ifeq ($(EIGEN_EXISTS),no)
	@echo "Eigen not found via pkg-config. Checking common locations..."
	@if [ ! -d "/usr/include/eigen3" ] && [ ! -d "/usr/local/include/eigen3" ]; then \
	    echo "Eigen3 not found. Please install it using:"; \
	    echo "  Ubuntu/Debian: sudo apt-get install libeigen3-dev"; \
	    echo "  Fedora/RHEL: sudo dnf install eigen3-devel"; \
	    echo "  Arch: sudo pacman -S eigen"; \
	    exit 1; \
	fi
	@echo "Eigen3 found at system location"
endif

# Create build directories
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)/core $(BUILD_DIR)/sdr $(BUILD_DIR)/dsp $(BUILD_DIR)/net $(BUILD_DIR)/web

# Compile individual object files.
# -MMD -MP auto-generates a per-object .d file listing the headers it includes,
# pulled in via the -include below. Without this, editing a header (e.g.
# src/core/types.hpp) did NOT recompile objects that include it, so a struct-
# layout change left stale .o files mixing old/new layouts -> out-of-bounds
# writes -> memory corruption (glibc mutex assertion). Now any header edit
# recompiles every dependent object.
# check-uws is an order-only prerequisite here and on $(TARGET): under -j,
# `all`'s prerequisites (check-deps and the target) otherwise build in
# PARALLEL, and the link races the uWebSockets clone/build. The fork's static
# library is order-only here too: its headers must exist before compiling.
$(BUILD_DIR)/%.o: src/%.cpp $(FLAGS_STAMP) | $(BUILD_DIR) check-uws $(RTLSDR_LIB)
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -IuWebSockets/src -IuWebSockets/uSockets/src -c $< -o $@

$(BUILD_DIR)/pffft/%.o: external/pffft/%.c external/pffft/pffft.h external/pffft/pffft_priv_impl.h $(wildcard external/pffft/simd/*.h) $(FLAGS_STAMP) | $(BUILD_DIR)
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(PFFFT_CC) $(PFFFT_CFLAGS) -c $< -o $@

# Windows process setup + manifest resource (see WIN_OBJECTS)
$(BUILD_DIR)/win/win_init.o: win/win_init.cpp win/posix_compat.h $(FLAGS_STAMP) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/win/app_res.o: win/app.rc win/app.manifest | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	windres --include-dir win -O coff $< -o $@

# The compiler flags the objects were built with (every platform). Rewritten
# (and so newer than every object) only when they differ, so a flags change
# recompiles everything, not only the files that changed - e.g. `make
# WIN_MARCH=-march=x86-64-v2` for a CPU without AVX2 (the objects of the
# default x86-64-v3 build would otherwise stay in the exe and still crash
# there), or new optimisation flags reaching an existing build
FLAGS_TEXT = $(strip $(CC) $(CFLAGS) | $(PFFFT_CC) $(PFFFT_CFLAGS))
ifneq ($(shell cat $(FLAGS_STAMP) 2>/dev/null),$(FLAGS_TEXT))
$(FLAGS_STAMP): flags-changed
endif
$(FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@echo '$(FLAGS_TEXT)' > $@

.PHONY: flags-changed
flags-changed:
ifeq ($(WINDOWS),1)
.PHONY: rtlsdr-reconfigure
rtlsdr-reconfigure:
endif
# forcing prerequisites of the fork's library (every platform, see RTLSDR_FIX_MARK)
.PHONY: rtlsdr-update rtlsdr-check
rtlsdr-update rtlsdr-check:

# Auto-generated header dependencies (one .d per object, alongside the .o).
# Leading '-' silences the harmless "no such file" on the very first build.
-include $(filter-out $(PFFFT_OBJECTS:.o=.d) $(WIN_OBJECTS:.o=.d),$(OBJECTS:.o=.d))

# Build the target
$(TARGET): $(OBJECTS) $(CONFIG_HEADER) $(RTLSDR_LIB) | check-uws
	@echo "Linking $(TARGET) with uWebSockets and Eigen..."
	@echo "Compiler: $(CC)"
	@echo "Architecture: $(ARCH)"
	@echo "Modules: core, sdr, dsp (with Eigen), net, web"
	$(CC) $(CFLAGS) -IuWebSockets/src -IuWebSockets/uSockets/src \
	    -o $(TARGET)$(LINK_TMP) $(OBJECTS) \
	    uWebSockets/uSockets/*.o $(LDFLAGS)
ifeq ($(WINDOWS),1)
	@mv -f $(TARGET)$(LINK_TMP) $(TARGET)
endif
	@echo "Build complete - Using eigenvalue decomposition for phase calibration!"
	@echo "Modular architecture with $(words $(SOURCES)) source files"
	@echo "Run with: ./$(TARGET)"

# Build the fork's static library (only that target - install.sh builds and
# installs the rtl_* tools). Clones the fork if ../librtlsdr is missing or is
# the empty placeholder directory (git clone refuses a non-empty one).
# (core.autocrlf=false: Git for Windows would check its scripts out with
# CRLF line ends; no effect elsewhere)
$(RTLSDR_LIB): $(RTLSDR_SRCS) $(RTLSDR_CHECK) $(RTLSDR_UPDATE) $(RTLSDR_RECONFIGURE)
	@if [ ! -f $(RTLSDR_DIR)/CMakeLists.txt ]; then \
	    echo "KrakenSDR librtlsdr fork not found - cloning into $(RTLSDR_DIR)"; \
	    git -c core.autocrlf=false clone $(RTLSDR_REPO) $(RTLSDR_DIR); \
	fi
	@if [ -n "$(RTLSDR_UPDATE)" ] && ! git -C $(RTLSDR_DIR) grep -q $(RTLSDR_FIX_MARK) HEAD -- src/librtlsdr.c; then \
	    echo "Updating the librtlsdr fork in $(RTLSDR_DIR) (it predates krakenrf/librtlsdr 578b43d)"; \
	    if git -C $(RTLSDR_DIR) fetch -q && git -C $(RTLSDR_DIR) grep -q $(RTLSDR_FIX_MARK) '@{u}' -- src/librtlsdr.c 2>/dev/null && \
	       git -C $(RTLSDR_DIR) merge-base --is-ancestor HEAD '@{u}'; then \
	        if [ "$(WINDOWS)" = 1 ] && git -C $(RTLSDR_DIR) diff HEAD -- src/librtlsdr.c | grep -q librtlsdr-windows.patch; then \
	            git -c core.autocrlf=false -C $(RTLSDR_DIR) checkout -- src/librtlsdr.c || exit 1; \
	        fi; \
	        git -c core.autocrlf=false -C $(RTLSDR_DIR) merge -q --ff-only '@{u}' || true; \
	    fi; \
	    git -C $(RTLSDR_DIR) grep -q $(RTLSDR_FIX_MARK) HEAD -- src/librtlsdr.c || { \
	        echo "Warning: could not update $(RTLSDR_DIR) to krakenrf/librtlsdr 578b43d or later"; \
	        mkdir -p $(dir $(RTLSDR_TRIED)) && git -C $(RTLSDR_DIR) rev-parse HEAD > $(RTLSDR_TRIED); }; \
	fi
	@grep -q $(RTLSDR_FIX_MARK) $(RTLSDR_DIR)/src/librtlsdr.c || \
	    { echo "$(RTLSDR_DIR) predates the librtlsdr fork heimdall needs (krakenrf/librtlsdr 578b43d, with $(RTLSDR_FIX_MARK)): $(if $(wildcard $(RTLSDR_DIR)/.git),update it (git -C $(RTLSDR_DIR) pull --ff-only - commit or stash any changes of yours there first) or )delete it to have it cloned again"; exit 1; }
	cmake -S $(RTLSDR_DIR) -B $(RTLSDR_DIR)/build $(RTLSDR_CMAKE_FLAGS)
	$(MAKE) -C $(RTLSDR_DIR)/build rtlsdr_static
	@touch $(RTLSDR_LIB)   # newer than RTLSDR_SRCS also when nothing recompiled

# Install uWebSockets
# Windows: uSockets on libuv (no epoll there), without LTO
USOCKETS_FLAGS = WITH_SSL=0
ifeq ($(WINDOWS),1)
USOCKETS_FLAGS = WITH_SSL=0 WITH_LIBUV=1 WITH_LTO=0 CC=gcc
endif
install-uws:
	@if [ ! -d uWebSockets ]; then \
		echo "Installing uWebSockets $(UWS_COMMIT) locally..."; \
		git -c core.autocrlf=false clone $(UWS_REPO) uWebSockets && \
		git -C uWebSockets checkout -q $(UWS_COMMIT) && \
		git -c core.autocrlf=false -C uWebSockets submodule update -q --init uSockets || exit 1; \
	fi
	@if ! ls uWebSockets/uSockets/*.o >/dev/null 2>&1; then \
		echo "Building uSockets..."; \
		$(MAKE) -C uWebSockets/uSockets $(USOCKETS_FLAGS) || exit 1; \
	else \
		echo "uWebSockets found locally"; \
	fi
	@echo ""

# Install system dependencies
deps:
	@echo "Installing system dependencies..."
	sudo apt-get update
	sudo apt-get install -y \
	    cmake \
	    libusb-1.0-0-dev \
	    libeigen3-dev \
	    build-essential \
	    git \
	    pkg-config \
	    libssl-dev \
	    libcrypto++-dev
	@echo "Dependencies installed (including Eigen3). The librtlsdr fork is"
	@echo "built from source and linked statically - no librtlsdr-dev needed."

# For Fedora/RHEL
deps-fedora:
	sudo dnf install -y \
	    cmake \
	    libusb1-devel \
	    eigen3-devel \
	    gcc-c++ \
	    git \
	    make \
	    openssl-devel

# For Arch Linux
deps-arch:
	sudo pacman -S \
	    cmake \
	    libusb \
	    eigen \
	    gcc \
	    git \
	    make \
	    openssl

# For macOS (Homebrew)
deps-macos:
	brew install cmake libusb eigen openssl@3 pkg-config

# Clean build artifacts
clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
	@echo "Build artifacts cleaned"

# Deep clean including uWebSockets
distclean: clean
	rm -rf uWebSockets
	@echo "All artifacts and dependencies removed"

# Rebuild everything
rebuild: clean all

# Run the program
run: $(TARGET)
	@echo "Starting Heimdall with eigenvalue decomposition..."
	@echo "=============================================="
	@echo "Web interface: http://localhost:8080"
	@echo "Phase calibration: Eigenvalue decomposition"
	@echo "Modules: core, sdr, dsp (Eigen), net, web"
	@echo "Stop with Ctrl+C"
	@echo "=============================================="
	./$(TARGET)

# Check installation status
status:
	@echo "=== Installation Status ==="
	@echo -n "uWebSockets: "
	@if [ -d "uWebSockets" ]; then \
	    echo "✓ Installed"; \
	else \
	    echo "✗ Not installed (run 'make install-uws')"; \
	fi
	@echo -n "RTL-SDR (KrakenSDR fork, static): "
	@if [ -f $(RTLSDR_LIB) ]; then \
	    echo "✓ Built ($(RTLSDR_LIB))"; \
	else \
	    echo "✗ Not built (run 'make' or ../install.sh)"; \
	fi
	@echo "FFT: pffft (vendored in external/pffft, no system FFT library)"
	@echo -n "Eigen3: "
	@if pkg-config --exists eigen3 2>/dev/null || [ -d "/usr/include/eigen3" ]; then \
	    echo "✓ Installed"; \
	else \
	    echo "✗ Not installed (run 'make deps')"; \
	fi
	@echo ""
	@echo "=== Source Structure ==="
	@echo "Core modules: $(words $(CORE_SOURCES)) files"
	@echo "SDR modules:  $(words $(SDR_SOURCES)) files" 
	@echo "DSP modules:  $(words $(DSP_SOURCES)) files (with Eigen)"
	@echo "Net modules:  $(words $(NET_SOURCES)) files"
	@echo "Web modules:  $(words $(WEB_SOURCES)) files"
	@echo "Total:        $(words $(SOURCES)) source files"

# Debug: show what will be compiled
debug:
	@echo "=== Debug Information ==="
	@echo "Target: $(TARGET)"
	@echo "Build directory: $(BUILD_DIR)"
	@echo "Compiler: $(CC)"
	@echo "Flags: $(CFLAGS)"
	@echo "Link flags: $(LDFLAGS)"
	@echo "Eigen support: $(EIGEN_EXISTS)"
	@echo ""
	@echo "Source files:"
	@for src in $(SOURCES); do echo "  $$src"; done
	@echo ""
	@echo "Object files:"
	@for obj in $(OBJECTS); do echo "  $$obj"; done

# Help
help:
	@echo "Heimdall with Eigenvalue Decomposition - Modular Build System"
	@echo "============================================================="
	@echo ""
	@echo "Quick start:"
	@echo "  make deps          # Install system dependencies (including Eigen)"
	@echo "  make              # Build (auto-installs uWebSockets)"
	@echo "  make run          # Build and run"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build the project (default)"
	@echo "  install-uws      - Install uWebSockets"
	@echo "  check-eigen      - Check Eigen3 installation"
	@echo "  deps             - Install system dependencies (Debian/Ubuntu)"
	@echo "  deps-fedora      - Install dependencies for Fedora/RHEL"
	@echo "  deps-arch        - Install dependencies for Arch Linux"
	@echo "  deps-macos       - Install dependencies for macOS (Homebrew)"
	@echo "  clean            - Remove build artifacts"
	@echo "  distclean        - Remove everything including uWebSockets"
	@echo "  rebuild          - Clean and rebuild"
	@echo "  run              - Build and run the program"
	@echo "  status           - Check installation and module status"
	@echo "  debug            - Show build configuration"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Modular Structure:"
	@echo "  src/core/        - Core utilities and types"
	@echo "  src/sdr/         - SDR device management and pipeline"
	@echo "  src/dsp/         - Signal processing with Eigen eigenvalue decomposition"
	@echo "  src/net/         - TCP servers and networking"
	@echo "  src/web/         - Web interface and HTML loading"
	@echo ""
	@echo "Phase Calibration:"
	@echo "  Using eigenvalue decomposition of spatial correlation matrix"
	@echo "  Provides more robust phase estimation than correlation peaks"
	@echo ""
	@echo "Architecture: $(ARCH)"
	@echo "Compiler: $(CC) (C++20)"
	@echo "Modules: $(words $(SOURCES)) source files"
	@echo ""

.PHONY: all check-deps check-uws check-eigen install-uws deps deps-fedora deps-arch deps-macos clean distclean rebuild run status debug help