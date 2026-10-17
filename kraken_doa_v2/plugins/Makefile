# Decoder plugins: every directory with a decoder.cpp becomes
# <dir>/build/decoder (its .cpp files + sdk/plugin_host.cpp + the shared
# digital-radio library lib/, which the linker only pulls in where used). Run from
# kraken_doa_v2 (`make` builds the plugins too) or directly:
#
#   make -C plugins                 # all plugins
#   make -C plugins PLUGIN=pocsag   # one plugin
#   make -C plugins clean
#
# Never more than -j3 on a Raspberry Pi (see kraken_doa_v2/CLAUDE.md).

CXX ?= g++
CC ?= gcc
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -pthread -Isdk -Ilib -I../external/pffft -isystem $(LIQUID_INC)
# + $(LIQUID_A): liquid-dsp built from source, static, without FFTW / libfec
# (../liquid.mk; built here too when kraken_doa's make hasn't yet)
LIBS = -lm -ldl

# FFT: pffft (kraken_doa_v2/external/pffft, through sdk/kraken_fft.hpp),
# compiled into lib/build/libkrakendig.a. It builds its scalar code unless
# told about NEON
PFFFT_DIR := ../external/pffft
PFFFT_CFLAGS = -O3 -Wno-unused-parameter

ARCH := $(shell uname -m)
ifneq ($(filter aarch64 armv7l x86_64,$(ARCH)),)
    CXXFLAGS += -march=native
    PFFFT_CFLAGS += -march=native
endif
ifeq ($(ARCH),aarch64)
    PFFFT_CFLAGS += -DPFFFT_ENABLE_NEON
endif
ifeq ($(ARCH),armv7l)
    PFFFT_CFLAGS += -mfpu=neon -DPFFFT_ENABLE_NEON
endif

ifdef PLUGIN
PLUGINS := $(PLUGIN)
else
PLUGINS := $(patsubst %/decoder.cpp,%,$(wildcard */decoder.cpp))
endif

TARGETS := $(addsuffix /build/decoder,$(PLUGINS))
HOST_OBJ := sdk/build/plugin_host.o
SDK_HDRS := sdk/kraken_plugin.hpp sdk/kraken_dsp.hpp sdk/kraken_fft.hpp
LIB_SRCS := $(wildcard lib/*.cpp)
PFFFT_OBJS := lib/build/pffft.o lib/build/pffft_common.o
LIB_OBJS := $(patsubst lib/%.cpp,lib/build/%.o,$(LIB_SRCS)) $(PFFFT_OBJS)
LIB_HDRS := $(wildcard lib/*.hpp)
LIB_A := lib/build/libkrakendig.a

# codec check (kraken_doa's "Digital Decoders" box): what the plugins find
CODECS := lib/build/codecs

.PHONY: all clean list
all: $(TARGETS) $(CODECS)

# liquid-dsp: LIQUID_A / LIQUID_INC (kraken_doa_v2/liquid-dsp/build-kraken)
LIQUID_ROOT := ..
include ../liquid.mk

$(CODECS): lib/tools/codecs.cpp $(LIB_A) $(LIB_HDRS) $(LIQUID_H) $(LIQUID_A)
	$(CXX) $(CXXFLAGS) -o $@.tmp $< $(LIB_A) $(LIQUID_A) $(LIBS)
	@mv -f $@.tmp $@

list:
	@echo $(PLUGINS)

$(HOST_OBJ): sdk/plugin_host.cpp $(SDK_HDRS) $(LIQUID_H)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

lib/build/%.o: lib/%.cpp $(LIB_HDRS) $(SDK_HDRS) $(LIQUID_H)
	@mkdir -p lib/build
	$(CXX) $(CXXFLAGS) -c $< -o $@

PFFFT_HDRS := $(PFFFT_DIR)/pffft.h $(PFFFT_DIR)/pffft_priv_impl.h $(wildcard $(PFFFT_DIR)/simd/*.h)
lib/build/pffft.o: $(PFFFT_DIR)/pffft.c $(PFFFT_HDRS)
	@mkdir -p lib/build
	$(CC) $(PFFFT_CFLAGS) -c $< -o $@
lib/build/pffft_common.o: $(PFFFT_DIR)/pffft_common.c $(PFFFT_HDRS)
	@mkdir -p lib/build
	$(CC) $(PFFFT_CFLAGS) -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	@rm -f $@
	ar rcs $@ $^

# plugins/<id>/CXXFLAGS: extra compiler flags for that plugin (e.g. -O3, last
# on the command line, so they win over the defaults)
extra_flags = $(shell cat $(1)/CXXFLAGS 2>/dev/null)

# Linked to a temporary name and renamed: a running copy (kraken_doa runs
# the plugin) keeps its old inode, and kraken_doa sees the new mtime and
# restarts the decoder on it.
.SECONDEXPANSION:
%/build/decoder: $$(wildcard $$*/*.cpp) $$(wildcard $$*/*.hpp) $$(wildcard $$*/*.h) $$(wildcard $$*/CXXFLAGS) $(HOST_OBJ) $(SDK_HDRS) $(LIB_A) $(LIB_HDRS) $(LIQUID_H) $(LIQUID_A)
	@test -f $*/decoder.cpp || { echo "no plugin '$*' (expected plugins/$*/decoder.cpp)"; exit 1; }
	@mkdir -p $*/build
	$(CXX) $(CXXFLAGS) $(call extra_flags,$*) -I$* -o $*/build/decoder.tmp $(wildcard $*/*.cpp) $(HOST_OBJ) $(LIB_A) $(LIQUID_A) $(LIBS)
	@mv -f $*/build/decoder.tmp $*/build/decoder
	@echo "built plugin $* -> $*/build/decoder"

clean:
	rm -rf sdk/build lib/build */build
