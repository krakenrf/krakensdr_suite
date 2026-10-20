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

# Windows (MSYS2 UCRT64 / MinGW-w64, see ../../WINDOWS.md): <dir>/build/decoder.exe,
# the same flags as kraken_doa_v2/Makefile's WINDOWS section, static exes
WINDOWS := $(if $(findstring mingw,$(shell $(CXX) -dumpmachine 2>/dev/null)),1,)
EXE :=
ifeq ($(WINDOWS),1)
EXE := .exe
WIN_MARCH ?= -march=x86-64-v3 -mtune=generic
WIN_ASFLAGS = -Wa,-muse-unaligned-vector-move
CXXFLAGS += -I../include $(WIN_MARCH) $(WIN_ASFLAGS) -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN \
            -DNOMINMAX -DNOGDI -D_USE_MATH_DEFINES -D_FILE_OFFSET_BITS=64 -DPFFFT_STATIC_DEFINE \
            -include ../win/posix_compat.h
# PFFFT_STATIC_DEFINE: pffft.h otherwise declares its API dllimport on _WIN32
PFFFT_CFLAGS += $(WIN_MARCH) $(WIN_ASFLAGS) -DPFFFT_STATIC_DEFINE
# dlfcn-win32 gives dlopen() for the mbelib DLL (dig_vocoder.cpp)
LIBS = -static -lm -ldl -lws2_32 -lwinmm -lbcrypt -luser32 -ladvapi32
# everything compiled depends on this stamp of WIN_MARCH (rule below):
# another WIN_MARCH recompiles everything, not only the files that changed
WIN_MARCH_STAMP := lib/build/.win_march
endif

ARCH := $(shell uname -m)
ifeq ($(WINDOWS),1)
else ifneq ($(filter aarch64 armv7l x86_64,$(ARCH)),)
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

TARGETS := $(addsuffix /build/decoder$(EXE),$(PLUGINS))
HOST_OBJ := sdk/build/plugin_host.o
SDK_HDRS := sdk/kraken_plugin.hpp sdk/kraken_dsp.hpp sdk/kraken_fft.hpp
LIB_SRCS := $(wildcard lib/*.cpp)
PFFFT_OBJS := lib/build/pffft.o lib/build/pffft_common.o
# kraken_doa's child-process API (../include/utils/process.hpp) + file helpers:
# the TETRA codec helpers dig_vocoder.cpp starts
ifeq ($(WINDOWS),1)
KPROC_SRC := ../src/utils/process_win.cpp
else
KPROC_SRC := ../src/utils/process_posix.cpp
endif
KPROC_OBJS := lib/build/kproc.o lib/build/fs_compat.o
LIB_OBJS := $(patsubst lib/%.cpp,lib/build/%.o,$(LIB_SRCS)) $(PFFFT_OBJS) $(KPROC_OBJS)
LIB_HDRS := $(wildcard lib/*.hpp)
LIB_A := lib/build/libkrakendig.a
# Windows: process setup (binary stdin/stdout - the plugin protocol - timers,
# Winsock) and the manifest in every plugin exe (../win/)
WIN_OBJS :=
ifeq ($(WINDOWS),1)
WIN_OBJS := sdk/build/win_init.o sdk/build/app_res.o
endif

# codec check (kraken_doa's "Digital Decoders" box): what the plugins find
CODECS := lib/build/codecs$(EXE)

.PHONY: all clean list
all: $(TARGETS) $(CODECS)

# liquid-dsp: LIQUID_A / LIQUID_INC (kraken_doa_v2/liquid-dsp/build-kraken)
LIQUID_ROOT := ..
include ../liquid.mk

$(CODECS): lib/tools/codecs.cpp $(LIB_A) $(LIB_HDRS) $(LIQUID_H) $(LIQUID_A) $(WIN_OBJS) $(WIN_MARCH_STAMP)
	$(CXX) $(CXXFLAGS) -o $@.tmp $< $(WIN_OBJS) $(LIB_A) $(LIQUID_A) $(LIBS)
	@mv -f $@.tmp $@

list:
	@echo $(PLUGINS)

$(HOST_OBJ): sdk/plugin_host.cpp $(SDK_HDRS) $(LIQUID_H) $(WIN_MARCH_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

lib/build/%.o: lib/%.cpp $(LIB_HDRS) $(SDK_HDRS) $(LIQUID_H) $(WIN_MARCH_STAMP)
	@mkdir -p lib/build
	$(CXX) $(CXXFLAGS) -c $< -o $@

lib/build/kproc.o: $(KPROC_SRC) ../include/utils/process.hpp $(WIN_MARCH_STAMP)
	@mkdir -p lib/build
	$(CXX) $(CXXFLAGS) -I../include -c $< -o $@

lib/build/fs_compat.o: ../src/utils/fs_compat.cpp ../include/utils/fs_compat.hpp $(WIN_MARCH_STAMP)
	@mkdir -p lib/build
	$(CXX) $(CXXFLAGS) -I../include -c $< -o $@

sdk/build/win_init.o: ../win/win_init.cpp ../win/posix_compat.h $(WIN_MARCH_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

sdk/build/app_res.o: ../win/app.rc ../win/app.manifest
	@mkdir -p $(dir $@)
	windres --include-dir ../win -O coff $< -o $@

PFFFT_HDRS := $(PFFFT_DIR)/pffft.h $(PFFFT_DIR)/pffft_priv_impl.h $(wildcard $(PFFFT_DIR)/simd/*.h)
lib/build/pffft.o: $(PFFFT_DIR)/pffft.c $(PFFFT_HDRS) $(WIN_MARCH_STAMP)
	@mkdir -p lib/build
	$(CC) $(PFFFT_CFLAGS) -c $< -o $@
lib/build/pffft_common.o: $(PFFFT_DIR)/pffft_common.c $(PFFFT_HDRS) $(WIN_MARCH_STAMP)
	@mkdir -p lib/build
	$(CC) $(PFFFT_CFLAGS) -c $< -o $@

ifeq ($(WINDOWS),1)
# The WIN_MARCH the plugins were built with. Rewritten (and so newer than
# everything built) only when it differs, e.g. `make WIN_MARCH=-march=x86-64-v2`
# for a CPU without AVX2: objects of the default x86-64-v3 build would
# otherwise stay in the exes and still crash there
ifneq ($(shell cat $(WIN_MARCH_STAMP) 2>/dev/null),$(strip $(WIN_MARCH)))
$(WIN_MARCH_STAMP): win-march-changed
endif
$(WIN_MARCH_STAMP):
	@mkdir -p $(dir $@)
	@echo '$(strip $(WIN_MARCH))' > $@

.PHONY: win-march-changed
win-march-changed:
endif

$(LIB_A): $(LIB_OBJS)
	@rm -f $@
	ar rcs $@ $^

# plugins/<id>/CXXFLAGS: extra compiler flags for that plugin (e.g. -O3, last
# on the command line, so they win over the defaults)
# Only harmless flags are taken (optimisation, target, defines): a flag such as
# -B <dir> or -fplugin=... makes the compiler RUN programs - and the AI
# assistant may write plugin folders (its own builds are sandboxed, a later
# `make` by the user is not)
CXXFLAGS_ALLOWED = -O0 -O1 -O2 -O3 -Os -Ofast -ffast-math -funroll-loops -fno-math-errno -ftree-vectorize \
                   -fno-tree-vectorize -march=% -mtune=% -mcpu=% -D%
extra_flags = $(filter $(CXXFLAGS_ALLOWED),$(shell cat $(1)/CXXFLAGS 2>/dev/null))

# Linked to a temporary name and renamed: a running copy (kraken_doa runs
# the plugin) keeps its old inode, and kraken_doa sees the new mtime and
# restarts the decoder on it. (Windows can't overwrite a running .exe, but
# MSYS2's mv moves the busy file aside first; g++ -o x.tmp adds no .exe.)
.SECONDEXPANSION:
%/build/decoder$(EXE): $$(wildcard $$*/*.cpp) $$(wildcard $$*/*.hpp) $$(wildcard $$*/*.h) $$(wildcard $$*/CXXFLAGS) $(HOST_OBJ) $(SDK_HDRS) $(LIB_A) $(LIB_HDRS) $(LIQUID_H) $(LIQUID_A) $(WIN_OBJS) $(WIN_MARCH_STAMP)
	@test -f $*/decoder.cpp || { echo "no plugin '$*' (expected plugins/$*/decoder.cpp)"; exit 1; }
	@mkdir -p $*/build
	$(CXX) $(CXXFLAGS) $(call extra_flags,$*) -I$* -o $*/build/decoder.tmp $(wildcard $*/*.cpp) $(HOST_OBJ) $(WIN_OBJS) $(LIB_A) $(LIQUID_A) $(LIBS)
	@mv -f $*/build/decoder.tmp $*/build/decoder$(EXE)
	@echo "built plugin $* -> $*/build/decoder$(EXE)"

# Windows: the test-signal generators written in C++ (<id>/tools/*.cpp, the
# Debug Playback steps of sdk/tools/make_testsignal.py) prebuilt with the
# plugins' flags into <id>/tools/build/<name>.exe, which make_testsignal.py
# runs - a PC running kraken_doa usually has no compiler. (Linux: the script
# compiles them itself when first needed.)
GEN_BUILD_DIRS :=
ifeq ($(WINDOWS),1)
GEN_SRCS := $(foreach p,$(PLUGINS),$(wildcard $(p)/tools/*.cpp))
GEN_TARGETS := $(foreach s,$(GEN_SRCS),$(dir $(s))build/$(basename $(notdir $(s)))$(EXE))
GEN_BUILD_DIRS := */tools/build
gen_plugin = $(firstword $(subst /, ,$(1)))
all: $(GEN_TARGETS)
$(GEN_TARGETS): %$(EXE): $$(subst /build/,/,$$*).cpp $$(wildcard $$(call gen_plugin,$$*)/*.hpp $$(call gen_plugin,$$*)/*/*.hpp) $(SDK_HDRS) $(LIB_A) $(LIB_HDRS) $(LIQUID_H) $(LIQUID_A) $(WIN_OBJS) $(WIN_MARCH_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(call gen_plugin,$*) -o $@.tmp $< $(WIN_OBJS) $(LIB_A) $(LIQUID_A) $(LIBS)
	@mv -f $@.tmp $@
	@echo "built generator $< -> $@"
endif

clean:
	rm -rf sdk/build lib/build */build $(GEN_BUILD_DIRS)
