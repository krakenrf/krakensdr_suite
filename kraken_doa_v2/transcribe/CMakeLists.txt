# kraken_whisper - the speech-to-text helper of kraken_doa's Voice to Text.
# Built by setup.sh (which clones whisper.cpp at the pinned commit next to
# this file first): cmake -S . -B build && cmake --build build --target kraken_whisper
cmake_minimum_required(VERSION 3.16)
project(kraken_whisper C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# whisper.cpp as a static library, nothing else from it
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_SDL2 OFF CACHE BOOL "" FORCE)
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/whisper.cpp/CMakeLists.txt)
    message(FATAL_ERROR "whisper.cpp/ is missing - run transcribe/setup.sh, which clones the pinned version")
endif()
add_subdirectory(whisper.cpp EXCLUDE_FROM_ALL)

add_executable(kraken_whisper kraken_whisper.cpp)
target_include_directories(kraken_whisper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(kraken_whisper PRIVATE whisper)
target_compile_options(kraken_whisper PRIVATE -Wall -Wextra)

# Windows (MSYS2 UCRT64 / MinGW-w64, setup.sh's MinGW branch): built like the
# suite's other .exe files (see ../Makefile) - win_init.cpp (binary stdin /
# stdout, which carry the voice2text wire - the C runtime would turn every
# \n byte into \r\n; Winsock, timer resolution) + the manifest (UTF-8 file
# names: a model path with an accent), the POSIX shims force-included, the
# same defines, and linked statically: kraken_doa starts it from
# transcribe/build/, where no MSYS2 DLL is found. process_win.cpp:
# kproc::lower_own_priority (Windows has no setpriority / nice).
if(WIN32)
    enable_language(RC)
    set(KRAKEN_WIN ${CMAKE_CURRENT_SOURCE_DIR}/../win)
    target_sources(kraken_whisper PRIVATE ${KRAKEN_WIN}/win_init.cpp ${KRAKEN_WIN}/app.rc
                   ${CMAKE_CURRENT_SOURCE_DIR}/../src/utils/process_win.cpp)
    # windres finds app.rc's app.manifest there
    target_include_directories(kraken_whisper PRIVATE $<$<COMPILE_LANGUAGE:RC>:${KRAKEN_WIN}>)
    target_compile_definitions(kraken_whisper PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                               NOGDI _USE_MATH_DEFINES _FILE_OFFSET_BITS=64)
    target_compile_options(kraken_whisper PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${KRAKEN_WIN}/posix_compat.h>")
    target_link_options(kraken_whisper PRIVATE -static)
    target_link_libraries(kraken_whisper PRIVATE ws2_32 winmm)
endif()
