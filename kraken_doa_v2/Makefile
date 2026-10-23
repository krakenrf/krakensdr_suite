# Makefile for KrakenSDR DoA with MUSIC DoA and Raw Data Buffering
CXX = g++
# -O3 -ffast-math without -ffinite-math-only (it folds std::isfinite /
# std::isnan to constants). It was -Ofast = this + -ffinite-math-only +
# -fallow-store-data-races (stores other threads may race with); the same
# speed on the Pi 5 - root CLAUDE.md *Compiler flags*
CXXFLAGS = -std=c++20 -O3 -ffast-math -fno-finite-math-only -Wall -Wextra -pthread
INCLUDES = -isystem $(LIQUID_INC) -Iinclude -Iplugins/sdk -Iexternal/pffft -IuWebSockets/src -IuWebSockets/uSockets/src -I/usr/include/eigen3
LDFLAGS = -L/usr/local/lib
# liquid-dsp: built from source as a static library without FFTW / libfec
# (liquid.mk - Debian's libliquid1 links both); linked as $(LIQUID_A)
LIBS = -lopus -lssl -lcrypto -lz -lsqlite3

# FFT: pffft, vendored in external/pffft (plain C; plugins/sdk/kraken_fft.hpp
# wraps it). It builds its scalar code unless told about NEON
CC = gcc
PFFFT_CFLAGS = -O3 -march=native -Wno-unused-parameter

# Windows: a native build with MSYS2's UCRT64 toolchain (MinGW-w64 GCC; see
# ../WINDOWS.md) - run `make` in the UCRT64 shell, or ../build_windows.bat.
# Everything Windows-specific in this Makefile (and plugins/Makefile,
# liquid.mk) is under WINDOWS; the Linux build is unchanged.
WINDOWS := $(if $(findstring mingw,$(shell $(CXX) -dumpmachine 2>/dev/null)),1,)
ifeq ($(WINDOWS),1)
    # An exe that runs on other PCs too: AVX2 (x86-64-v3) rather than
    # -march=native (this machine's AVX-512 = an illegal instruction
    # elsewhere); `make WIN_MARCH=-march=native` for this PC only.
    # -muse-unaligned-vector-move: GCC can't 32-byte-align the Win64 stack
    # (GCC bug 54412), so an AVX spill with an aligned move could fault
    WIN_MARCH ?= -march=x86-64-v3 -mtune=generic
    export WIN_MARCH
    WIN_ASFLAGS = -Wa,-muse-unaligned-vector-move
    # Windows 10+ API, windows.h without min/max macros and GDI (its ERROR
    # macro collides with voice2text_wire.hpp), M_PI & co. under -std=c++20,
    # 64-bit off_t / stat sizes (recordings > 2 GB), libuv under uSockets,
    # and the POSIX functions MinGW lacks (win/posix_compat.h, force-included)
    # PFFFT_STATIC_DEFINE: pffft.h otherwise declares its API dllimport on _WIN32
    WIN_DEFS = -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNOGDI \
               -D_USE_MATH_DEFINES -D_FILE_OFFSET_BITS=64 -DPFFFT_STATIC_DEFINE
    INCLUDES = -isystem $(LIQUID_INC) -Iinclude -Iplugins/sdk -Iexternal/pffft -IuWebSockets/src \
               -IuWebSockets/uSockets/src $(shell pkg-config --cflags eigen3)
    PFFFT_CFLAGS = -O3 $(WIN_MARCH) $(WIN_ASFLAGS) -DPFFFT_STATIC_DEFINE -Wno-unused-parameter
    # one self-contained exe (no MSYS2 DLLs needed): static libstdc++ /
    # winpthread / OpenSSL / opus / zlib / sqlite / libuv + the system libs
    LDFLAGS = -static
    LIBS = -lopus -lssl -lcrypto -lz -lsqlite3 $(shell pkg-config --static --libs libuv) \
           -lws2_32 -lcrypt32 -lbcrypt -lgdi32 -luser32 -ladvapi32 -lwinmm
endif

# macOS (Apple clang + Homebrew: eigen, openssl@3, opus, cmake, pkg-config -
# `make deps-macos`): the Linux build plus the shims of mac/posix_compat.h
# (force-included) and Homebrew's include / library paths (OpenSSL is
# keg-only). Everything macOS-specific is under MACOS (plugins/Makefile and
# liquid.mk too)
MACOS := $(if $(filter Darwin,$(shell uname -s)),1,)
ifeq ($(MACOS),1)
# one deployment target for everything (this macOS version, as CMake picks
# it for librtlsdr / liquid-dsp): no "built for newer macOS" link warnings
export MACOSX_DEPLOYMENT_TARGET ?= $(shell sw_vers -productVersion | cut -d. -f1-2)
endif
ifeq ($(MACOS),1)
    BREW_PREFIX := $(or $(shell brew --prefix 2>/dev/null),/usr/local)
    OPENSSL_PREFIX := $(or $(shell brew --prefix openssl@3 2>/dev/null),$(BREW_PREFIX)/opt/openssl@3)
    CXXFLAGS += -include mac/posix_compat.h
    INCLUDES += -I$(BREW_PREFIX)/include/eigen3 -I$(OPENSSL_PREFIX)/include -I$(BREW_PREFIX)/include
    LDFLAGS = -L$(OPENSSL_PREFIX)/lib -L$(BREW_PREFIX)/lib
    # platform.cpp's keep_system_awake (an IOKit power assertion)
    LIBS += -framework IOKit -framework CoreFoundation
endif

# Detect ARM architecture for NEON optimizations
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
ifeq ($(WINDOWS),1)
    CXXFLAGS += $(WIN_MARCH) $(WIN_ASFLAGS) $(WIN_DEFS) -DLIBUS_USE_LIBUV -include win/posix_compat.h
    $(info Windows build (MinGW-w64): $(WIN_MARCH))
else ifeq ($(ARCH),aarch64)
    # ARM 64-bit (Raspberry Pi 4/5, etc.; Apple Silicon: AARCH64_CPU above)
ifeq ($(MACOS),1)
    CXXFLAGS += $(AARCH64_CPU)
    PFFFT_CFLAGS := $(filter-out -march=native,$(PFFFT_CFLAGS)) $(AARCH64_CPU)
else
    CXXFLAGS += -march=native -mtune=native
endif
    PFFFT_CFLAGS += -DPFFFT_ENABLE_NEON
    $(info Detected ARM64 architecture - NEON optimizations will be enabled)
else ifeq ($(ARCH),armv7l)
    # ARM 32-bit
    CXXFLAGS += -march=native -mtune=native -mfpu=neon
    PFFFT_CFLAGS += -mfpu=neon -DPFFFT_ENABLE_NEON
    $(info Detected ARM32 architecture - NEON optimizations will be enabled)
else ifeq ($(findstring arm,$(ARCH)),arm)
    # Other ARM variants
    CXXFLAGS += -march=native -mtune=native
    $(info Detected ARM architecture variant: $(ARCH))
else
    # x86 and others
    CXXFLAGS += -march=native -mtune=native
    $(info Detected $(ARCH) architecture - using scalar optimizations)
endif

# Check if uWebSockets exists locally
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
    rm -f uWebSockets/uSockets/*.o && rm -rf obj dep && echo ok)
ifneq ($(UWS_SWITCH),ok)
$(error could not switch uWebSockets to $(UWS_COMMIT) - delete the uWebSockets folder and run make again)
endif
endif
endif

# Directories
SRCDIR = src
INCDIR = include
OBJDIR = obj
DEPDIR = dep
TARGET = kraken_doa
ifeq ($(WINDOWS),1)
TARGET = kraken_doa.exe
# linked to a temporary name, then moved: Windows can't overwrite a running
# exe, MSYS2's mv moves the busy one aside (a rebuild while the stack runs)
LINK_TMP = .tmp
endif
# every object depends on this stamp of the compiler flags (rule below):
# other flags recompile everything, not only the files that changed
FLAGS_STAMP = $(OBJDIR)/.flags

# Source files (updated with IQ converter)
SIGNAL_PROCESSING_SRCS = \
	$(SRCDIR)/signal_processing/fft_processor.cpp \
	$(SRCDIR)/signal_processing/noise_floor.cpp \
	$(SRCDIR)/signal_processing/fm_demodulator.cpp \
	$(SRCDIR)/signal_processing/audio_mixer.cpp \
	$(SRCDIR)/signal_processing/music_processor.cpp \
	$(SRCDIR)/signal_processing/shared_decimator.cpp \
	$(SRCDIR)/signal_processing/beamformer.cpp

NETWORKING_SRCS = \
	$(SRCDIR)/networking/binary_message.cpp \
	$(SRCDIR)/networking/data_receiver.cpp \
	$(SRCDIR)/networking/tcp_client.cpp \
	$(SRCDIR)/networking/gpsd_client.cpp \
	$(SRCDIR)/networking/web_mapper.cpp \
	$(SRCDIR)/networking/websocket_server.cpp

# the decoders themselves are plugins (plugins/, built by the plugins target)
DIGITAL_SRCS = \
	$(SRCDIR)/digital/dig_report.cpp \
	$(SRCDIR)/digital/dig_plugin.cpp \
	$(SRCDIR)/digital/digital_decoder.cpp

UTILS_SRCS = \
	$(SRCDIR)/utils/ring_buffer.cpp \
	$(SRCDIR)/utils/raw_data_buffer.cpp \
	$(SRCDIR)/utils/system_stats.cpp \
	$(SRCDIR)/utils/status_dashboard.cpp \
	$(SRCDIR)/utils/iq_converter.cpp \
	$(SRCDIR)/utils/disk_util.cpp \
	$(SRCDIR)/utils/platform.cpp \
	$(SRCDIR)/utils/fs_compat.cpp \
	$(SRCDIR)/utils/tls_compat.cpp
# child processes (include/utils/process.hpp): one backend per platform
ifeq ($(WINDOWS),1)
UTILS_SRCS += $(SRCDIR)/utils/process_win.cpp
else
UTILS_SRCS += $(SRCDIR)/utils/process_posix.cpp
endif

APP_SRCS = \
	$(SRCDIR)/main.cpp \
	$(SRCDIR)/station_info.cpp \
	$(SRCDIR)/doa_logger.cpp \
	$(SRCDIR)/channel_manager.cpp \
	$(SRCDIR)/control_handler.cpp \
	$(SRCDIR)/settings_store.cpp \
	$(SRCDIR)/message_builders.cpp \
	$(SRCDIR)/decimator_manager.cpp \
	$(SRCDIR)/scanner_manager.cpp \
	$(SRCDIR)/continuous_scanner.cpp \
	$(SRCDIR)/ai_manager.cpp \
	$(SRCDIR)/ai_mcp.cpp \
	$(SRCDIR)/ai_capture.cpp \
	$(SRCDIR)/ai_map.cpp \
	$(SRCDIR)/ai_sandbox.cpp \
	$(SRCDIR)/geo_address.cpp \
	$(SRCDIR)/geo_http.cpp \
	$(SRCDIR)/incidents.cpp \
	$(SRCDIR)/saved_freqs.cpp \
	$(SRCDIR)/sf_scan_plan.cpp \
	$(SRCDIR)/sf_scanner.cpp \
	$(SRCDIR)/decoder_log.cpp \
	$(SRCDIR)/rdf_engine.cpp \
	$(SRCDIR)/rdf_mapper.cpp \
	$(SRCDIR)/talker_doa.cpp \
	$(SRCDIR)/array_cal.cpp \
	$(SRCDIR)/voice2text.cpp \
	$(SRCDIR)/playback.cpp \
	$(SRCDIR)/sigmf.cpp \
	$(SRCDIR)/recorder.cpp \
	$(SRCDIR)/recorder_audio.cpp

ALL_SRCS = $(SIGNAL_PROCESSING_SRCS) $(NETWORKING_SRCS) $(DIGITAL_SRCS) $(UTILS_SRCS) $(APP_SRCS)

# Object files (+ pffft, C)
PFFFT_OBJS = $(OBJDIR)/pffft/pffft.o $(OBJDIR)/pffft/pffft_common.o
OBJS = $(ALL_SRCS:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o) $(PFFFT_OBJS)
# Windows: process setup (binary file mode, timers, Winsock - win/win_init.cpp)
# and the manifest (UTF-8 code page, long paths) in every exe
ifeq ($(WINDOWS),1)
WIN_OBJS = $(OBJDIR)/win/win_init.o $(OBJDIR)/win/app_res.o
OBJS += $(WIN_OBJS)
endif

# Dependency files
DEPS = $(ALL_SRCS:$(SRCDIR)/%.cpp=$(DEPDIR)/%.d)

# Create object and dependency directories
OBJDIRS = $(OBJDIR) $(OBJDIR)/signal_processing $(OBJDIR)/networking $(OBJDIR)/digital $(OBJDIR)/utils $(OBJDIR)/pffft
DEPDIRS = $(DEPDIR) $(DEPDIR)/signal_processing $(DEPDIR)/networking $(DEPDIR)/digital $(DEPDIR)/utils

.PHONY: all clean distclean install deps deps-macos check-uws debug help print-% run status plugins whisper whisper-update

# Default target
all: check-uws $(OBJDIRS) $(DEPDIRS) $(TARGET) plugins whisper-update

# liquid-dsp: LIQUID_A / LIQUID_INC and the `liquid` target (static, pinned)
include liquid.mk

# 🗣 Voice to Text: the local speech-to-text helper (transcribe/, whisper.cpp)
# is opt-in: `make whisper` = transcribe/setup.sh (clone the pinned whisper.cpp,
# build, download the default model). Once set up, every `make` keeps it up
# to date (a failure only warns - kraken_doa runs without it).
# `make whisper WHISPER_GPU=auto|on|off`: Metal on Apple Silicon by default
# (transcribe/setup.sh; the choice is remembered for later builds)
whisper:
	@transcribe/setup.sh

whisper-update:
	@if [ -x transcribe/build/kraken_whisper$(if $(WINDOWS),.exe,) ]; then \
		transcribe/setup.sh --update || \
		echo "WARNING: the Voice to Text helper failed to build (transcribe/) - kraken_doa itself is fine"; \
	fi

# Decoder plugins (plugins/<id>/decoder.cpp -> plugins/<id>/build/decoder,
# see plugins/SDK.md). Separate executables: kraken_doa doesn't link them, so
# a plugin that doesn't compile only warns (-k builds the others).
plugins: $(LIQUID_A)
	@if [ -f plugins/Makefile ]; then \
		$(MAKE) --no-print-directory -k -C plugins -f Makefile || \
		echo "WARNING: a decoder plugin failed to build (see above) - kraken_doa itself is fine"; \
	fi

# Check and install uWebSockets locally (Windows: uSockets on libuv - there
# is no epoll - without LTO; core.autocrlf=false: Git for Windows would check
# scripts out with CRLF line ends, no effect elsewhere)
USOCKETS_FLAGS = WITH_OPENSSL=1
ifeq ($(WINDOWS),1)
USOCKETS_FLAGS = WITH_OPENSSL=1 WITH_LIBUV=1 WITH_LTO=0 CC=gcc CXX=g++
endif
ifeq ($(MACOS),1)
USOCKETS_FLAGS += CFLAGS=-I$(OPENSSL_PREFIX)/include CXXFLAGS=-I$(OPENSSL_PREFIX)/include
endif
check-uws:
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

# Create directories
$(OBJDIRS) $(DEPDIRS):
	mkdir -p $@

# Link target (order-only check-uws: under -j the link otherwise races the
# uWebSockets clone/build and fails on the missing uSockets/*.o). $^ ends
# with the liquid archive, after the objects that use it
$(TARGET): $(OBJS) $(LIQUID_A) | check-uws
	@echo "Linking $(TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@$(LINK_TMP) $^ uWebSockets/uSockets/*.o $(LDFLAGS) $(LIBS)
ifeq ($(WINDOWS),1)
	@mv -f $@$(LINK_TMP) $@
endif
	@echo "Build complete!"
	@echo "Architecture: $(ARCH)"
ifeq ($(findstring arm,$(ARCH)),arm)
	@echo "ARM NEON optimizations: ENABLED"
else ifeq ($(ARCH),aarch64)
	@echo "ARM64 NEON optimizations: ENABLED"
else
	@echo "Optimizations: Scalar with loop unrolling"
endif
	@echo "NEW: Pre-converted float IQ data for efficiency"
	@echo "NEW: ARM NEON optimized IQ conversion on ARM platforms"
	@echo "Run with: ./$(TARGET)"

# Compile source files and generate dependencies
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(LIQUID_H) $(FLAGS_STAMP) | $(OBJDIRS) $(DEPDIRS) check-uws
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -MF $(DEPDIR)/$*.d -c $< -o $@

$(OBJDIR)/pffft/%.o: external/pffft/%.c external/pffft/pffft.h external/pffft/pffft_priv_impl.h $(wildcard external/pffft/simd/*.h) $(FLAGS_STAMP) | $(OBJDIRS)
	@echo "Compiling $<..."
	$(CC) $(PFFFT_CFLAGS) -c $< -o $@

# Windows process setup + manifest resource (see WIN_OBJS)
$(OBJDIR)/win/win_init.o: win/win_init.cpp win/posix_compat.h $(FLAGS_STAMP) | $(OBJDIRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/win/app_res.o: win/app.rc win/app.manifest | $(OBJDIRS)
	@mkdir -p $(dir $@)
	windres --include-dir win -O coff $< -o $@

# The flags the objects were built with (CXXFLAGS incl. WIN_MARCH, pffft's).
# Rewritten (and so newer than every object) only when they differ: a
# Makefile with other flags (a build from before keeps its old objects
# otherwise), or on Windows `make WIN_MARCH=-march=x86-64-v2` for a CPU
# without AVX2 - the objects of the default x86-64-v3 build would stay in the
# exe and still crash there (liquid.mk rebuilds liquid-dsp itself;
# plugins/Makefile has a stamp of its own). It was WIN_MARCH on Windows only
FLAGS_TEXT = $(strip $(CXX) $(CXXFLAGS) | $(CC) $(PFFFT_CFLAGS))
ifneq ($(shell cat $(FLAGS_STAMP) 2>/dev/null),$(FLAGS_TEXT))
$(FLAGS_STAMP): flags-changed
endif
$(FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@echo '$(FLAGS_TEXT)' > $@

.PHONY: flags-changed
flags-changed:

# Include dependency files (only if they exist and we're not cleaning)
ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)
-include $(DEPS)
endif

# Clean build artifacts
clean:
	rm -rf $(OBJDIR) $(DEPDIR) $(TARGET)
	@if [ -f plugins/Makefile ]; then $(MAKE) --no-print-directory -C plugins -f Makefile clean; fi
	@echo "Build artifacts cleaned"

# Complete clean including uWebSockets and liquid-dsp
distclean: clean
	rm -rf uWebSockets liquid-dsp
	@echo "All artifacts including uWebSockets and liquid-dsp removed"

# Install system dependencies only (Ubuntu/Debian)
deps:
	@echo "Installing system dependencies..."
	sudo apt-get update
	sudo apt-get install -y build-essential cmake pkg-config git
	sudo apt-get install -y libeigen3-dev
	sudo apt-get install -y libssl-dev libz-dev libsqlite3-dev
	@echo "System dependencies installed!"
	@echo "uWebSockets and liquid-dsp will be fetched and built locally when you run 'make'"

# macOS (Homebrew)
deps-macos:
	brew install cmake pkg-config eigen openssl@3 opus

# Install target (optional system-wide installation)
install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/
	sudo chmod +x /usr/local/bin/$(TARGET)
	@echo "Installed to /usr/local/bin/$(TARGET)"

# Debug build
debug: CXXFLAGS += -DDEBUG -g -O0
debug: $(TARGET)
	@echo "Debug build complete with symbols"

# ARM-specific debug build
debug-arm: CXXFLAGS += -DDEBUG -g -O0 -DARM_NEON_DEBUG
debug-arm: $(TARGET)
	@echo "ARM debug build complete with NEON debug output"

# Force reinstall uWebSockets
reinstall-uws:
	rm -rf uWebSockets
	$(MAKE) check-uws

# Check build status
status:
	@echo "=== Build Status ==="
	@echo "Architecture: $(ARCH)"
ifeq ($(findstring arm,$(ARCH)),arm)
	@echo "ARM NEON: ✓ Will be enabled"
else ifeq ($(ARCH),aarch64)
	@echo "ARM64 NEON: ✓ Will be enabled"
else
	@echo "SIMD: Scalar optimizations with loop unrolling"
endif
	@echo ""
	@echo -n "uWebSockets: "
	@if [ -d "uWebSockets" ]; then \
	    echo "✓ Installed locally"; \
	else \
	    echo "✗ Not installed (will be installed on build)"; \
	fi
	@echo "FFT: pffft (vendored in external/pffft)"
	@echo -n "liquid-dsp: "
	@if [ -f "$(LIQUID_A)" ]; then \
	    echo "✓ Built locally ($(LIQUID_A), static, without FFTW / libfec)"; \
	else \
	    echo "✗ Not built yet (built on 'make', or 'make liquid')"; \
	fi
	@echo -n "Eigen3: "
	@if pkg-config --exists eigen3 2>/dev/null || [ -d "/usr/include/eigen3" ]; then \
	    echo "✓ Found"; \
	else \
	    echo "✗ Not found (run 'make deps')"; \
	fi
	@echo ""
	@echo "=== File Status ==="
	@echo "Source files found: $(words $(ALL_SRCS))"
	@echo "  Signal processing: $(words $(SIGNAL_PROCESSING_SRCS))"
	@echo "  Networking: $(words $(NETWORKING_SRCS))"
	@echo "  Utilities: $(words $(UTILS_SRCS)) (including IQ converter)"
	@echo "  Application: $(words $(APP_SRCS))"
	@echo ""
	@echo "NEW: iq_converter.cpp - Optimized IQ data conversion"
	@echo "NEW: Pre-converted float IQ data in raw_data_buffer"
	@echo "UPDATED: data_receiver.cpp - Early IQ conversion"
	@echo ""
	@echo "=== Dependency Status ==="
	@if [ -d "$(DEPDIR)" ]; then \
	    echo "Dependency files: $$(find $(DEPDIR) -name '*.d' | wc -l) files"; \
	else \
	    echo "Dependency files: None (will be generated on build)"; \
	fi
	@echo "Object files: $$(find $(OBJDIR) -name '*.o' 2>/dev/null | wc -l) files" 2>/dev/null || echo "Object files: 0 files"

# List source files (useful for debugging)
list-sources:
	@echo "=== Source Files ==="
	@echo "Signal Processing:"
	@for file in $(SIGNAL_PROCESSING_SRCS); do echo "  $$file"; done
	@echo ""
	@echo "Networking:"
	@for file in $(NETWORKING_SRCS); do echo "  $$file"; done
	@echo ""
	@echo "Utilities:"
	@for file in $(UTILS_SRCS); do echo "  $$file"; done
	@echo ""
	@echo "Application:"
	@for file in $(APP_SRCS); do echo "  $$file"; done

# Check for missing source files
check-sources:
	@echo "=== Checking Source Files ==="
	@missing=0; \
	for file in $(ALL_SRCS); do \
	    if [ ! -f "$$file" ]; then \
	        echo "✗ Missing: $$file"; \
	        missing=$$((missing + 1)); \
	    fi; \
	done; \
	if [ $$missing -eq 0 ]; then \
	    echo "✓ All $(words $(ALL_SRCS)) source files found"; \
	else \
	    echo "✗ $$missing source files missing"; \
	    exit 1; \
	fi

# Print variables for debugging
print-%:
	@echo $* = $($*)

# Run the program
run: $(TARGET)
	@echo "Starting KrakenSDR DoA with optimized IQ conversion..."
	@echo "Architecture: $(ARCH)"
ifeq ($(findstring arm,$(ARCH)),arm)
	@echo "ARM NEON optimizations: ACTIVE"
else ifeq ($(ARCH),aarch64)
	@echo "ARM64 NEON optimizations: ACTIVE"
else
	@echo "Using scalar optimizations"
endif
	@echo ""
	@echo "Features:"
	@echo "  • Pre-converted float IQ data"
	@echo "  • Optimized IQ conversion (NEON on ARM)"
	@echo "  • Raw data buffering with bounded queue"
	@echo "  • Automatic old data cleanup"
	@echo "  • Separate decimation processor thread"
	@echo ""
	@echo "Web interface: https://localhost:8070"
	./$(TARGET)

# Force rebuild everything
rebuild: clean all

# Build with verbose output
verbose: CXXFLAGS += -v
verbose: $(TARGET)
	@echo "Verbose build complete"

# Quick verification build (compile only, no linking)
verify:
	@echo "=== Verification Build (Compile Only) ==="
	@$(MAKE) -k $(OBJS) || true
	@echo "=== Verification Complete ==="

# Benchmark build (with profiling support)
benchmark: CXXFLAGS += -pg -fno-omit-frame-pointer
benchmark: $(TARGET)
	@echo "Benchmark build complete (use gprof for profiling)"

# Help
help:
	@echo "KrakenSDR DoA with MUSIC DoA and Optimized IQ Conversion"
	@echo "======================================================"
	@echo ""
	@echo "Quick start:"
	@echo "  make deps         - Install system dependencies"  
	@echo "  make              - Build (auto-detects ARM and enables NEON)"
	@echo "  make run          - Build and run"
	@echo ""
	@echo "Build targets:"
	@echo "  all               - Build the project (default)"
	@echo "  debug             - Build with debug symbols"
	@echo "  debug-arm         - ARM debug build with NEON debug output"
	@echo "  benchmark         - Build with profiling support"
	@echo "  verbose           - Build with verbose compiler output"
	@echo "  verify            - Compile only (no linking) for verification"
	@echo "  clean             - Remove build artifacts"
	@echo "  distclean         - Remove everything including uWebSockets"
	@echo "  rebuild           - Clean and build"
	@echo "  whisper           - Set up Voice to Text (local speech-to-text, transcribe/setup.sh)"
	@echo ""
	@echo "Dependencies:"
	@echo "  deps              - Install system packages (Ubuntu/Debian)"
	@echo "  check-uws         - Install uWebSockets locally"
	@echo "  liquid            - Build liquid-dsp locally (static, without FFTW / libfec)"
	@echo "  reinstall-uws     - Force reinstall uWebSockets"
	@echo ""
	@echo "Information:"
	@echo "  status            - Check dependency and build status"
	@echo "  list-sources      - List all source files"
	@echo "  check-sources     - Verify all source files exist"
	@echo "  info              - Show build configuration"
	@echo ""
	@echo "Other:"
	@echo "  install           - Install to system (/usr/local/bin)"
	@echo "  run               - Build and run program"
	@echo "  help              - Show this help"
	@echo ""
	@echo "ARCHITECTURE:"
	@echo "  Current: $(ARCH)"
ifeq ($(findstring arm,$(ARCH)),arm)
	@echo "  ARM NEON optimizations will be automatically enabled"
else ifeq ($(ARCH),aarch64)
	@echo "  ARM64 NEON optimizations will be automatically enabled"
else
	@echo "  Scalar optimizations with loop unrolling will be used"
endif
	@echo ""
	@echo "OPTIMIZATIONS:"
	@echo "  • Early IQ conversion (uint8 to float) at packet reception"
	@echo "  • ARM NEON SIMD for IQ conversion on ARM platforms"
	@echo "  • 4x loop unrolling for scalar conversion"
	@echo "  • Pre-converted data stored to avoid duplicate conversions"
	@echo "  • Optimized for Raspberry Pi 4/5 and similar ARM devices"
	@echo ""
	@echo "Key Files:"
	@echo "  utils/iq_converter.cpp             - NEW: Optimized IQ conversion"
	@echo "  utils/raw_data_buffer.cpp          - UPDATED: Float IQ storage"
	@echo "  networking/data_receiver.cpp       - UPDATED: Early conversion"
	@echo "  signal_processing/shared_decimator - Next: Accept float input"
	@echo ""
	@echo "Features:"
	@echo "  - Automatic ARM/NEON detection"
	@echo "  - Architecture-specific optimizations"
	@echo "  - No manual flags needed for ARM"
	@echo "  - Parallel builds with 'make -j'"
	@echo ""
	@echo "Note: The build system automatically detects ARM architecture"
	@echo "      and enables NEON optimizations when available."

# Show compiler and flag information
info:
	@echo "=== Build Configuration ==="
	@echo "Architecture: $(ARCH)"
	@echo "Compiler: $(CXX)"
	@echo "Flags: $(CXXFLAGS)"
	@echo "Includes: $(INCLUDES)"
	@echo "Libraries: $(LIBS)"
	@echo "Sources: $(words $(ALL_SRCS)) files"
	@echo "Objects: $(words $(OBJS)) files"
	@echo "Dependencies: $(words $(DEPS)) files"
	@echo ""
	@echo "=== Optimization Status ==="
ifeq ($(findstring arm,$(ARCH)),arm)
	@echo "ARM NEON: ENABLED (detected ARM architecture)"
	@echo "IQ Conversion: Using NEON SIMD instructions"
	@echo "Processing: 8 complex samples per iteration"
else ifeq ($(ARCH),aarch64)
	@echo "ARM64 NEON: ENABLED (detected ARM64 architecture)"
	@echo "IQ Conversion: Using NEON SIMD instructions"
	@echo "Processing: 8 complex samples per iteration"
else
	@echo "SIMD: Not available on $(ARCH)"
	@echo "IQ Conversion: Using optimized scalar with 4x unrolling"
	@echo "Processing: 4 complex samples per iteration"
endif
	@echo ""
	@echo "=== Performance Features ==="
	@echo "• Early IQ conversion at packet reception"
	@echo "• Single conversion point (no duplicates)"
	@echo "• Pre-converted float storage"
	@echo "• Optimized memory access patterns"
	@echo "• Cache-friendly data layout"

# Test build (useful for CI/CD)
test-build: check-sources verify
	@echo "=== Test Build Complete ==="

# ARM-specific test
test-arm:
	@echo "=== ARM Configuration Test ==="
	@echo "Architecture detected: $(ARCH)"
	@echo "Compiler flags: $(CXXFLAGS)"
	@echo ""
	@echo "Testing NEON detection in code..."
	@echo "#include <stdio.h>" > test_neon.cpp
	@echo "#ifdef __ARM_NEON" >> test_neon.cpp
	@echo "int main() { printf(\"ARM NEON: AVAILABLE\\n\"); return 0; }" >> test_neon.cpp
	@echo "#else" >> test_neon.cpp
	@echo "int main() { printf(\"ARM NEON: NOT AVAILABLE\\n\"); return 0; }" >> test_neon.cpp
	@echo "#endif" >> test_neon.cpp
	@$(CXX) $(CXXFLAGS) test_neon.cpp -o test_neon
	@./test_neon
	@rm -f test_neon test_neon.cpp
	@echo "=== Test Complete ==="
