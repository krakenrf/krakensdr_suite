# Makefile for KrakenSDR DoA with MUSIC DoA and Raw Data Buffering
CXX = g++
CXXFLAGS = -std=c++20 -Ofast -Wall -Wextra -pthread
INCLUDES = -Iinclude -IuWebSockets/src -IuWebSockets/uSockets/src -I/usr/include/eigen3
LDFLAGS = -L/usr/local/lib
LIBS = -lfftw3f -lliquid -lopus -lssl -lcrypto -lz -lsqlite3

# Detect ARM architecture for NEON optimizations
ARCH := $(shell uname -m)
ifeq ($(ARCH),aarch64)
    # ARM 64-bit (Raspberry Pi 4/5, etc.)
    CXXFLAGS += -march=native -mtune=native
    $(info Detected ARM64 architecture - NEON optimizations will be enabled)
else ifeq ($(ARCH),armv7l)
    # ARM 32-bit
    CXXFLAGS += -march=native -mtune=native -mfpu=neon
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

# Directories
SRCDIR = src
INCDIR = include
OBJDIR = obj
DEPDIR = dep
TARGET = kraken_doa

# Source files (updated with IQ converter)
SIGNAL_PROCESSING_SRCS = \
	$(SRCDIR)/signal_processing/fft_processor.cpp \
	$(SRCDIR)/signal_processing/fm_demodulator.cpp \
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
	$(SRCDIR)/utils/iq_converter.cpp

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
	$(SRCDIR)/geo_address.cpp \
	$(SRCDIR)/geo_http.cpp \
	$(SRCDIR)/incidents.cpp \
	$(SRCDIR)/decoder_log.cpp \
	$(SRCDIR)/rdf_engine.cpp \
	$(SRCDIR)/rdf_mapper.cpp \
	$(SRCDIR)/talker_doa.cpp

ALL_SRCS = $(SIGNAL_PROCESSING_SRCS) $(NETWORKING_SRCS) $(DIGITAL_SRCS) $(UTILS_SRCS) $(APP_SRCS)

# Object files
OBJS = $(ALL_SRCS:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)

# Dependency files
DEPS = $(ALL_SRCS:$(SRCDIR)/%.cpp=$(DEPDIR)/%.d)

# Create object and dependency directories
OBJDIRS = $(OBJDIR) $(OBJDIR)/signal_processing $(OBJDIR)/networking $(OBJDIR)/digital $(OBJDIR)/utils
DEPDIRS = $(DEPDIR) $(DEPDIR)/signal_processing $(DEPDIR)/networking $(DEPDIR)/digital $(DEPDIR)/utils

.PHONY: all clean distclean install deps check-uws debug help print-% run status plugins

# Default target
all: check-uws $(OBJDIRS) $(DEPDIRS) $(TARGET) plugins

# Decoder plugins (plugins/<id>/decoder.cpp -> plugins/<id>/build/decoder,
# see plugins/SDK.md). Separate executables: kraken_doa doesn't link them, so
# a plugin that doesn't compile only warns (-k builds the others).
plugins:
	@if [ -f plugins/Makefile ]; then \
		$(MAKE) --no-print-directory -k -C plugins || \
		echo "WARNING: a decoder plugin failed to build (see above) - kraken_doa itself is fine"; \
	fi

# Check and install uWebSockets locally
check-uws:
ifeq ($(UWS_EXISTS),no)
	@echo "Installing uWebSockets locally..."
	git clone --recursive https://github.com/uNetworking/uWebSockets
	cd uWebSockets/uSockets && WITH_OPENSSL=1 make
	@echo "uWebSockets installed in local directory!"
else
	@echo "uWebSockets found locally"
endif

# Create directories
$(OBJDIRS) $(DEPDIRS):
	mkdir -p $@

# Link target (order-only check-uws: under -j the link otherwise races the
# uWebSockets clone/build and fails on the missing uSockets/*.o)
$(TARGET): $(OBJS) | check-uws
	@echo "Linking $(TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ uWebSockets/uSockets/*.o $(LDFLAGS) $(LIBS)
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
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIRS) $(DEPDIRS) check-uws
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -MF $(DEPDIR)/$*.d -c $< -o $@

# Include dependency files (only if they exist and we're not cleaning)
ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)
-include $(DEPS)
endif

# Clean build artifacts
clean:
	rm -rf $(OBJDIR) $(DEPDIR) $(TARGET)
	@if [ -f plugins/Makefile ]; then $(MAKE) --no-print-directory -C plugins clean; fi
	@echo "Build artifacts cleaned"

# Complete clean including uWebSockets
distclean: clean
	rm -rf uWebSockets
	@echo "All artifacts including uWebSockets removed"

# Install system dependencies only (Ubuntu/Debian)
deps:
	@echo "Installing system dependencies..."
	sudo apt-get update
	sudo apt-get install -y build-essential cmake pkg-config git
	sudo apt-get install -y libfftw3-dev libliquid-dev libeigen3-dev
	sudo apt-get install -y libssl-dev libz-dev libsqlite3-dev
	@echo "System dependencies installed!"
	@echo "uWebSockets will be installed locally when you run 'make'"

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
	@echo -n "FFTW3: "
	@if pkg-config --exists fftw3f 2>/dev/null; then \
	    echo "✓ Found"; \
	else \
	    echo "✗ Not found (run 'make deps')"; \
	fi
	@echo -n "liquid-dsp: "
	@if pkg-config --exists liquid 2>/dev/null || ldconfig -p | grep -q libliquid; then \
	    echo "✓ Found"; \
	else \
	    echo "✗ Not found (run 'make deps')"; \
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
	@echo ""
	@echo "Dependencies:"
	@echo "  deps              - Install system packages (Ubuntu/Debian)"
	@echo "  check-uws         - Install uWebSockets locally"
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
