# krakensdr.exe, the Windows launcher (krakensdr.cpp - run.sh's job on Windows),
# built into the repository root next to heimdall_v2/ and kraken_doa_v2/.
# MSYS2 UCRT64 shell:  make -C windows      (build.sh / build_windows.bat run it)

CXX = g++
TARGET = ../krakensdr.exe
# baseline x86-64 (not WIN_MARCH): the launcher must run on any PC, also
# one whose CPU lacks what the apps were built for - and say so
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -municode -march=x86-64 -mtune=generic \
           -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
# self-contained, like the apps (user32: the hidden window that gets logoff / shutdown)
LIBS = -static -lws2_32 -lshell32 -luser32

.PHONY: all clean
all: $(TARGET)

# linked to a temporary name, then moved: Windows can't overwrite a running
# exe, MSYS2's mv moves the busy one aside (a rebuild while the stack runs -
# its supervisors are krakensdr.exe)
$(TARGET): krakensdr.cpp build/app_res.o
	$(CXX) $(CXXFLAGS) -o $@.tmp krakensdr.cpp build/app_res.o $(LIBS)
	@mv -f $@.tmp $@

# the apps' manifest (UTF-8 code page, Windows 10/11)
build/app_res.o: ../heimdall_v2/win/app.rc ../heimdall_v2/win/app.manifest
	@mkdir -p build
	windres --include-dir ../heimdall_v2/win -O coff $< -o $@

clean:
	rm -rf build $(TARGET) $(TARGET).tmp
