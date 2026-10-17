# liquid-dsp, built from source as a STATIC library without FFTW and libfec.
#
# Debian's libliquid1 is compiled against FFTW (GPL-2.0+) and libfec
# (LGPL-2.1), so every program linked to it loads FFTW. liquid-dsp itself is
# MIT; kraken_doa and the plugins only use its filters, resamplers, AGC and
# filter design (none of its FFT- or libfec-based objects), and liquid has
# its own FFT for the few places that need one. Built here, nothing GPL is
# linked into kraken_doa or a plugin.
#
# Used by kraken_doa_v2/Makefile (LIQUID_ROOT = .), plugins/Makefile (..),
# CMakeLists.txt and the Dockerfile (`make -f liquid.mk liquid`). Pinned to
# v1.7.0 = the version Debian 13 ships, so the DSP is the code the receiver
# was tested with; liquid's CMake build of that version never looks for FFTW
# or libfec. Override for testing: make LIQUID_COMMIT=<sha>.

LIQUID_REPO ?= https://github.com/jgaeddert/liquid-dsp
LIQUID_COMMIT ?= 4dda702cb81cf96155e614299aab6180ff09740e
LIQUID_ROOT ?= .
LIQUID_DIR := $(LIQUID_ROOT)/liquid-dsp
LIQUID_BUILD := $(LIQUID_DIR)/build-kraken
LIQUID_A := $(LIQUID_BUILD)/libliquid.a
# <liquid/liquid.h> resolves here, ahead of a system libliquid-dev header.
# Included with -isystem (liquid.h warns about its own deprecated objects,
# which a system header doesn't) - so the dependency files don't list it and
# what includes it depends on LIQUID_H explicitly
LIQUID_INC := $(LIQUID_BUILD)/include
LIQUID_H := $(LIQUID_INC)/liquid/liquid.h
LIQUID_LOG := $(LIQUID_DIR)/build-kraken.log

# NEON / SSE dot products (liquid's CMake picks them); -march=native as the
# rest of the build. armv7 needs -mfpu=neon for liquid to find NEON
LIQUID_ARCH := $(shell uname -m)
ifeq ($(LIQUID_ARCH),armv7l)
LIQUID_CFLAGS ?= -march=native -mfpu=neon
else ifneq ($(filter aarch64 x86_64,$(LIQUID_ARCH)),)
LIQUID_CFLAGS ?= -march=native
endif

# A library built from another commit (an older pin) is rebuilt: the build
# (archive + header) removed here, before any rule runs, so everything that
# includes the header is compiled again too
ifneq ($(wildcard $(LIQUID_A)),)
ifneq ($(shell cat $(LIQUID_BUILD)/commit 2>/dev/null),$(LIQUID_COMMIT))
$(info liquid-dsp was built from another commit - rebuilding $(LIQUID_COMMIT))
LIQUID_SWITCH := $(shell rm -rf $(LIQUID_BUILD))
endif
endif

.PHONY: liquid
liquid: $(LIQUID_A)

# Made by the archive's recipe. Order-only: a rebuilt archive doesn't make
# the header - and every object - out of date; the header's own time (the
# checkout's, kept by cp -p) decides
$(LIQUID_H): | $(LIQUID_A) ;

# The archive is written last (renamed into place): it existing = a complete
# build. `+`: liquid's own make joins this make's jobserver (-j3 stays -j3)
$(LIQUID_A):
	@echo "liquid-dsp: building $(LIQUID_COMMIT) (static, without FFTW / libfec; log: $(LIQUID_LOG))"
	@if ! git -C $(LIQUID_DIR) cat-file -e $(LIQUID_COMMIT)^{commit} 2>/dev/null; then \
		{ [ -d $(LIQUID_DIR)/.git ] || { rm -rf $(LIQUID_DIR) && git init -q $(LIQUID_DIR) && \
		  git -C $(LIQUID_DIR) remote add origin $(LIQUID_REPO); }; } && \
		git -C $(LIQUID_DIR) fetch -q --depth 1 origin $(LIQUID_COMMIT) || \
		{ echo "could not fetch liquid-dsp $(LIQUID_COMMIT) - delete $(LIQUID_DIR) and run make again"; exit 1; }; \
	fi
	@git -C $(LIQUID_DIR) -c advice.detachedHead=false checkout -q --force $(LIQUID_COMMIT)
	@rm -rf $(LIQUID_BUILD)
	@cmake -S $(LIQUID_DIR) -B $(LIQUID_BUILD) -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_EXAMPLES=OFF -DBUILD_AUTOTESTS=OFF -DBUILD_BENCHMARKS=OFF \
		-DCMAKE_C_FLAGS="$(LIQUID_CFLAGS)" > $(LIQUID_LOG) 2>&1 || \
		{ tail -20 $(LIQUID_LOG); exit 1; }
	+@cmake --build $(LIQUID_BUILD) --target liquid >> $(LIQUID_LOG) 2>&1 || \
		{ grep -E 'error|Error' $(LIQUID_LOG) | head -20; tail -5 $(LIQUID_LOG); exit 1; }
	@find $(LIQUID_BUILD)/CMakeFiles -path '*.dir/*' -name '*.o' | LC_ALL=C sort > $(LIQUID_BUILD)/objects
	@rm -f $@.tmp && ar rcs $@.tmp $$(cat $(LIQUID_BUILD)/objects)
	@mkdir -p $(LIQUID_INC)/liquid && cp -p $(LIQUID_DIR)/include/liquid.h $(LIQUID_INC)/liquid/
	@echo $(LIQUID_COMMIT) > $(LIQUID_BUILD)/commit
	@mv -f $@.tmp $@
	@echo "liquid-dsp: $@ ($$(wc -l < $(LIQUID_BUILD)/objects) objects)"
