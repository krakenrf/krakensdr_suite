# Heimdall Standalone Makefile with uWebSockets and Eigen - Modular Version
# Enhanced with Eigenvalue Decomposition for Phase Calibration

CC = g++
TARGET = heimdall
CONFIG_HEADER = config.h

# Source files organized by module
CORE_SOURCES = src/core/utils.cpp src/core/settings.cpp src/core/forward_comp.cpp src/core/status_dashboard.cpp
SDR_SOURCES = src/sdr/sdr_device.cpp src/sdr/sdr_init.cpp src/sdr/sdr_pipeline.cpp src/sdr/pipeline_control.cpp src/sdr/downconverter.cpp src/sdr/kerberos_gpio.cpp
DSP_SOURCES = src/dsp/fft_plan.cpp src/dsp/correlation.cpp src/dsp/compensation.cpp
NET_SOURCES = src/net/tcp_common.cpp src/net/tcp_data_server.cpp src/net/tcp_control_server.cpp src/net/rtl_tcp_server.cpp
WEB_SOURCES = src/web/html_loader.cpp src/web/web_server.cpp
MAIN_SOURCE = src/main.cpp

# All source files
SOURCES = $(CORE_SOURCES) $(SDR_SOURCES) $(DSP_SOURCES) $(NET_SOURCES) $(WEB_SOURCES) $(MAIN_SOURCE)

# Object files (in build directory)
BUILD_DIR = build
OBJECTS = $(SOURCES:src/%.cpp=$(BUILD_DIR)/%.o)

# Check if uWebSockets is installed
UWS_EXISTS := $(shell test -d uWebSockets && echo yes || echo no)

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

# C++20 required for uWebSockets
CFLAGS = -Wall -Wextra -std=c++20 -D_GNU_SOURCE -DUWS_NO_ZLIB
CFLAGS += -I. -Isrc -Isrc/core -Isrc/sdr -Isrc/dsp -Isrc/net -Isrc/web -Iexternal
CFLAGS += -I$(RTLSDR_DIR)/include   # the fork's headers, never a system rtl-sdr.h
LDFLAGS = $(RTLSDR_LIB) $(LIBUSB_LIBS) -lfftw3f -lpthread -lm -lz

# Add Eigen include path
ifeq ($(EIGEN_EXISTS),yes)
    CFLAGS += $(shell pkg-config --cflags eigen3)
else
    # Fallback to common Eigen location
    CFLAGS += -I/usr/include/eigen3
endif

# Add SSL if available (optional for uWebSockets)
LDFLAGS += -lssl -lcrypto

# Architecture detection
ARCH := $(shell uname -m)

ifeq ($(ARCH),aarch64)
    CFLAGS += -O3 -march=armv8-a -mtune=cortex-a72 -ftree-vectorize
else ifeq ($(ARCH),x86_64)
    CFLAGS += -O3 -march=native -mtune=native
else
    CFLAGS += -O2
endif

# Default target
all: check-deps $(TARGET)

# Check dependencies
check-deps: check-uws check-eigen

# Check if uWebSockets is installed
check-uws:
ifeq ($(UWS_EXISTS),no)
	@echo "uWebSockets not found. Installing..."
	@$(MAKE) install-uws
endif

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
$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR) check-uws $(RTLSDR_LIB)
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -IuWebSockets/src -IuWebSockets/uSockets/src -c $< -o $@

# Auto-generated header dependencies (one .d per object, alongside the .o).
# Leading '-' silences the harmless "no such file" on the very first build.
-include $(OBJECTS:.o=.d)

# Build the target
$(TARGET): $(OBJECTS) $(CONFIG_HEADER) $(RTLSDR_LIB) | check-uws
	@echo "Linking $(TARGET) with uWebSockets and Eigen..."
	@echo "Compiler: $(CC)"
	@echo "Architecture: $(ARCH)"
	@echo "Modules: core, sdr, dsp (with Eigen), net, web"
	$(CC) $(CFLAGS) -IuWebSockets/src -IuWebSockets/uSockets/src \
	    -o $(TARGET) $(OBJECTS) \
	    uWebSockets/uSockets/*.o $(LDFLAGS)
	@echo "Build complete - Using eigenvalue decomposition for phase calibration!"
	@echo "Modular architecture with $(words $(SOURCES)) source files"
	@echo "Run with: ./$(TARGET)"

# Build the fork's static library (only that target - install.sh builds and
# installs the rtl_* tools). Clones the fork if ../librtlsdr is missing or is
# the empty placeholder directory (git clone refuses a non-empty one).
$(RTLSDR_LIB):
	@if [ ! -f $(RTLSDR_DIR)/CMakeLists.txt ]; then \
	    echo "KrakenSDR librtlsdr fork not found - cloning into $(RTLSDR_DIR)"; \
	    git clone $(RTLSDR_REPO) $(RTLSDR_DIR); \
	fi
	cmake -S $(RTLSDR_DIR) -B $(RTLSDR_DIR)/build -DCMAKE_BUILD_TYPE=Release \
	    -DLINK_RTLTOOLS_AGAINST_STATIC_LIB=ON -DINSTALL_UDEV_RULES=ON
	$(MAKE) -C $(RTLSDR_DIR)/build rtlsdr_static

# Install uWebSockets
install-uws:
	@echo "=== Installing uWebSockets ==="
	@echo "Cloning repository..."
	git clone --recursive https://github.com/uNetworking/uWebSockets
	@echo "Building uSockets..."
	cd uWebSockets/uSockets && WITH_SSL=0 make
	@echo "uWebSockets installation complete!"
	@echo ""

# Install system dependencies
deps:
	@echo "Installing system dependencies..."
	sudo apt-get update
	sudo apt-get install -y \
	    cmake \
	    libusb-1.0-0-dev \
	    libfftw3-dev \
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
	    fftw3-devel \
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
	    fftw \
	    eigen \
	    gcc \
	    git \
	    make \
	    openssl

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
	@echo -n "FFTW3: "
	@if pkg-config --exists fftw3f 2>/dev/null; then \
	    echo "✓ Installed"; \
	else \
	    echo "✗ Not installed (run 'make deps')"; \
	fi
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

.PHONY: all check-deps check-uws check-eigen install-uws deps deps-fedora deps-arch clean distclean rebuild run status debug help