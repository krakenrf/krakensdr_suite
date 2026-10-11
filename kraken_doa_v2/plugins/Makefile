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
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -pthread -Isdk -Ilib
LIBS = -lliquid -lfftw3f -lm -ldl

ARCH := $(shell uname -m)
ifneq ($(filter aarch64 armv7l x86_64,$(ARCH)),)
    CXXFLAGS += -march=native
endif

ifdef PLUGIN
PLUGINS := $(PLUGIN)
else
PLUGINS := $(patsubst %/decoder.cpp,%,$(wildcard */decoder.cpp))
endif

TARGETS := $(addsuffix /build/decoder,$(PLUGINS))
HOST_OBJ := sdk/build/plugin_host.o
SDK_HDRS := sdk/kraken_plugin.hpp sdk/kraken_dsp.hpp
LIB_SRCS := $(wildcard lib/*.cpp)
LIB_OBJS := $(patsubst lib/%.cpp,lib/build/%.o,$(LIB_SRCS))
LIB_HDRS := $(wildcard lib/*.hpp)
LIB_A := lib/build/libkrakendig.a

# codec check (kraken_doa's "Digital Decoders" box): what the plugins find
CODECS := lib/build/codecs

.PHONY: all clean list
all: $(TARGETS) $(CODECS)

$(CODECS): lib/tools/codecs.cpp $(LIB_A) $(LIB_HDRS)
	$(CXX) $(CXXFLAGS) -o $@.tmp $< $(LIB_A) $(LIBS)
	@mv -f $@.tmp $@

list:
	@echo $(PLUGINS)

$(HOST_OBJ): sdk/plugin_host.cpp $(SDK_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

lib/build/%.o: lib/%.cpp $(LIB_HDRS) $(SDK_HDRS)
	@mkdir -p lib/build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	@rm -f $@
	ar rcs $@ $^

# A plugin can share another plugin's sources: plugins/<id>/USES names plugin
# ids whose .cpp files (except their decoder.cpp) are compiled in, with their
# folder on the include path (ism_wide = the ism decoders at 2.4 MHz).
uses = $(filter-out sdk lib $(1),$(shell cat $(1)/USES 2>/dev/null))
# plugins/<id>/CXXFLAGS: extra compiler flags for that plugin (e.g. -O3, last
# on the command line, so they win over the defaults)
extra_flags = $(shell cat $(1)/CXXFLAGS 2>/dev/null)
shared_srcs = $(filter-out %/decoder.cpp,$(foreach u,$(call uses,$(1)),$(wildcard $(u)/*.cpp)))
shared_deps = $(call shared_srcs,$(1)) $(foreach u,$(call uses,$(1)),$(wildcard $(u)/*.hpp $(u)/*.h))

# Linked to a temporary name and renamed: a running copy (kraken_doa runs
# the plugin) keeps its old inode, and kraken_doa sees the new mtime and
# restarts the decoder on it.
.SECONDEXPANSION:
%/build/decoder: $$(wildcard $$*/*.cpp) $$(wildcard $$*/*.hpp) $$(wildcard $$*/*.h) $$(wildcard $$*/USES $$*/CXXFLAGS) $$(call shared_deps,$$*) $(HOST_OBJ) $(SDK_HDRS) $(LIB_A) $(LIB_HDRS)
	@test -f $*/decoder.cpp || { echo "no plugin '$*' (expected plugins/$*/decoder.cpp)"; exit 1; }
	@mkdir -p $*/build
	$(CXX) $(CXXFLAGS) $(call extra_flags,$*) -I$* $(addprefix -I,$(call uses,$*)) -o $*/build/decoder.tmp $(wildcard $*/*.cpp) $(call shared_srcs,$*) $(HOST_OBJ) $(LIB_A) $(LIBS)
	@mv -f $*/build/decoder.tmp $*/build/decoder
	@echo "built plugin $* -> $*/build/decoder"

clean:
	rm -rf sdk/build lib/build */build
