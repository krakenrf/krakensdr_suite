cmake_minimum_required(VERSION 3.10)
project(heimdall)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Find Eigen3
if(WIN32)
    # MSYS2 ships Eigen 5, whose config file refuses a "3.3" request
    # (same-major-version rule) - any Eigen will do
    find_package(Eigen3 REQUIRED NO_MODULE)
else()
find_package(Eigen3 3.3 REQUIRED NO_MODULE)
endif()

# Find other dependencies using pkg-config
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# Windows: a native build with MSYS2's UCRT64 toolchain (MinGW-w64 GCC; run
# cmake in the UCRT64 shell, e.g. `cmake -G Ninja ..`), mirroring the
# Makefile's WINDOWS sections. Everything Windows-specific in this file is
# under if(WIN32); the Linux build is unchanged.
if(WIN32)
    # libuv: uSockets' event loop there (no epoll)
    pkg_check_modules(LIBUV REQUIRED libuv)
    # An exe that runs on other PCs too: AVX2 (x86-64-v3, Haswell / Zen and
    # later) rather than -march=native (an AVX-512 build machine's code
    # crashes elsewhere with an illegal instruction); -DHEIMDALL_WIN_MARCH=
    # -march=native for this PC only. -muse-unaligned-vector-move: GCC can't
    # align the stack to 32 bytes on Win64 (GCC bug 54412), so an AVX spill
    # with an aligned move could fault - the assembler emits unaligned moves.
    set(HEIMDALL_WIN_MARCH "-march=x86-64-v3;-mtune=generic" CACHE STRING "Windows -march / -mtune flags")
    set(HEIMDALL_WIN_ASFLAGS "-Wa,-muse-unaligned-vector-move")
endif()

# FFT: pffft, vendored in external/pffft (plain C, BSD-style licence; replaced
# FFTW 2026-10-17) - no system FFT library
add_library(pffft STATIC external/pffft/pffft.c external/pffft/pffft_common.c)
set_target_properties(pffft PROPERTIES C_STANDARD 99)
target_compile_options(pffft PRIVATE -O3)
if(WIN32)
    target_compile_options(pffft PRIVATE ${HEIMDALL_WIN_MARCH} ${HEIMDALL_WIN_ASFLAGS})
    # a static library: its header would declare the functions dllimport
    target_compile_definitions(pffft PUBLIC PFFFT_STATIC_DEFINE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|armv7|arm")
    # without it pffft silently builds its scalar code
    target_compile_definitions(pffft PRIVATE PFFFT_ENABLE_NEON)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7")
        target_compile_options(pffft PRIVATE -mfpu=neon)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        target_compile_options(pffft PRIVATE -mcpu=native)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    target_compile_options(pffft PRIVATE -march=native)
endif()

# KrakenSDR librtlsdr fork, linked STATICALLY so a distro librtlsdr0 (earlier
# in the loader's search path than /usr/local/lib) can never shadow it - see
# the Makefile. Built by ../install.sh or by `make` in this directory.
set(RTLSDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../librtlsdr)
if(WIN32)
    # the fork's CMake names the static library librtlsdr_static.a there
    # (librtlsdr.a would be its DLL's import library)
    set(RTLSDR_STATIC_LIB ${RTLSDR_DIR}/build/src/librtlsdr_static.a)
    if(NOT EXISTS "${RTLSDR_STATIC_LIB}")
        message(FATAL_ERROR "KrakenSDR librtlsdr fork not built (${RTLSDR_STATIC_LIB}). "
                            "Run `make ../librtlsdr/build/src/librtlsdr_static.a` in heimdall_v2 (UCRT64 shell).")
    endif()
    # the Makefile's requirement: the fork at krakenrf/librtlsdr 578b43d or
    # later (RTLSDR_FIX_MARK there) and a library built from the current
    # sources - that make updates an older clone and rebuilds the library;
    # configured again when the sources change
    file(STRINGS "${RTLSDR_DIR}/src/librtlsdr.c" RTLSDR_FIX_LINE REGEX "rtlsdr_finish_tune" LIMIT_COUNT 1)
    if(NOT RTLSDR_FIX_LINE)
        message(FATAL_ERROR "${RTLSDR_DIR} predates the librtlsdr fork heimdall needs (krakenrf/librtlsdr 578b43d). "
                            "Run `make ../librtlsdr/build/src/librtlsdr_static.a` in heimdall_v2 (UCRT64 shell) to update it.")
    endif()
    file(GLOB RTLSDR_SRCS ${RTLSDR_DIR}/src/*.c ${RTLSDR_DIR}/src/*.h ${RTLSDR_DIR}/include/*.h)
    foreach(src ${RTLSDR_SRCS})
        if("${src}" IS_NEWER_THAN "${RTLSDR_STATIC_LIB}")
            message(FATAL_ERROR "${RTLSDR_STATIC_LIB} is older than ${src}. "
                                "Run `make ../librtlsdr/build/src/librtlsdr_static.a` in heimdall_v2 (UCRT64 shell).")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RTLSDR_SRCS} ${RTLSDR_STATIC_LIB})
else()
set(RTLSDR_STATIC_LIB ${RTLSDR_DIR}/build/src/librtlsdr.a)
if(NOT EXISTS "${RTLSDR_STATIC_LIB}")
    message(FATAL_ERROR "KrakenSDR librtlsdr fork not built (${RTLSDR_STATIC_LIB}). "
                        "Run ../install.sh, or `make ../librtlsdr/build/src/librtlsdr.a` in heimdall_v2.")
endif()
# the fork at krakenrf/librtlsdr 578b43d or later, as on Windows (the
# Makefile's RTLSDR_FIX_MARK: its fast drag calls and the stalled control OUT
# retry); the Makefile fast-forwards an older clone, install.sh pulls it
file(STRINGS "${RTLSDR_DIR}/src/librtlsdr.c" RTLSDR_FIX_LINE REGEX "rtlsdr_finish_tune" LIMIT_COUNT 1)
if(NOT RTLSDR_FIX_LINE)
    message(FATAL_ERROR "${RTLSDR_DIR} predates the librtlsdr fork heimdall needs (krakenrf/librtlsdr 578b43d). "
                        "Run ../install.sh, or `make ../librtlsdr/build/src/librtlsdr.a` in heimdall_v2, to update it "
                        "(git -C ../librtlsdr pull --ff-only - commit or stash any changes of yours there first).")
endif()
# a library built from the current sources, as on Windows: heimdall compiles
# against the fork's current headers, and a header with a new call
# (rtlsdr_set_center_freq_fast) next to an older library fails the link
file(GLOB RTLSDR_SRCS ${RTLSDR_DIR}/src/*.c ${RTLSDR_DIR}/src/*.h ${RTLSDR_DIR}/include/*.h)
foreach(src ${RTLSDR_SRCS})
    if("${src}" IS_NEWER_THAN "${RTLSDR_STATIC_LIB}")
        message(FATAL_ERROR "${RTLSDR_STATIC_LIB} is older than ${src}. "
                            "Run ../install.sh, or `make ../librtlsdr/build/src/librtlsdr.a` in heimdall_v2.")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RTLSDR_SRCS} ${RTLSDR_STATIC_LIB})
endif()

# Check for uWebSockets
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/uWebSockets")
    if(WIN32)
        # uSockets on libuv, without LTO - the objects linked below
        message(FATAL_ERROR "uWebSockets not found. Run `make install-uws` in heimdall_v2 (UCRT64 shell), "
                            "which builds uSockets with WITH_LIBUV=1 WITH_LTO=0 WITH_SSL=0 CC=gcc")
    endif()
    message(FATAL_ERROR "uWebSockets not found. Please run: git clone --recursive https://github.com/uNetworking/uWebSockets && cd uWebSockets/uSockets && make")
endif()

# Source files
set(CORE_SOURCES
    src/core/utils.cpp
    src/core/settings.cpp
    src/core/forward_comp.cpp
    src/core/status_dashboard.cpp
    src/core/platform.cpp
    src/core/fs_compat.cpp
)

set(SDR_SOURCES
    src/sdr/sdr_device.cpp
    src/sdr/sdr_init.cpp
    src/sdr/sdr_pipeline.cpp
    src/sdr/pipeline_control.cpp
    src/sdr/kerberos_gpio.cpp
    src/sdr/downconverter.cpp
)

set(DSP_SOURCES
    src/dsp/fft_plan.cpp
    src/dsp/correlation.cpp
    src/dsp/compensation.cpp
)

set(NET_SOURCES
    src/net/tcp_common.cpp
    src/net/tcp_data_server.cpp
    src/net/tcp_control_server.cpp
    src/net/rtl_tcp_server.cpp
)

set(WEB_SOURCES
    src/web/html_loader.cpp
    src/web/web_server.cpp
)

set(MAIN_SOURCE
    src/main.cpp
)

# All sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${SDR_SOURCES}
    ${DSP_SOURCES}
    ${NET_SOURCES}
    ${WEB_SOURCES}
    ${MAIN_SOURCE}
)

# Windows: process setup (binary file mode, timers, Winsock - win/win_init.cpp)
# and the manifest resource (UTF-8 code page, long paths - win/app.rc)
if(WIN32)
    enable_language(RC)
    # app.rc names app.manifest relative to win/
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --include-dir=${CMAKE_CURRENT_SOURCE_DIR}/win")
    list(APPEND ALL_SOURCES win/win_init.cpp win/app.rc)
endif()

# Create executable
add_executable(heimdall ${ALL_SOURCES})

# Include directories
target_include_directories(heimdall PRIVATE
    ${CMAKE_SOURCE_DIR}              # Add root directory for config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdr
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/web
    ${CMAKE_CURRENT_SOURCE_DIR}/external
    ${CMAKE_CURRENT_SOURCE_DIR}/uWebSockets/src
    ${CMAKE_CURRENT_SOURCE_DIR}/uWebSockets/uSockets/src
    ${RTLSDR_DIR}/include
)

# Add Eigen3 include directories
target_include_directories(heimdall PRIVATE ${EIGEN3_INCLUDE_DIR})

# Compile definitions
target_compile_definitions(heimdall PRIVATE
    _GNU_SOURCE
    UWS_NO_ZLIB
)

# Compile options
target_compile_options(heimdall PRIVATE
    -Wall
    -Wextra
)

if(WIN32)
    # Windows 10+ API, windows.h without min/max macros and GDI (its ERROR
    # macro), M_PI & co. under -std=c++20, 64-bit off_t / stat sizes, libuv
    # under uSockets, and the POSIX functions MinGW lacks (win/posix_compat.h,
    # force-included into every C++ file)
    target_compile_definitions(heimdall PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        NOGDI
        _USE_MATH_DEFINES
        _FILE_OFFSET_BITS=64
        LIBUS_USE_LIBUV
    )
    target_compile_options(heimdall PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/win/posix_compat.h>"
    )
endif()

# Architecture-specific optimizations
if(WIN32)
    target_compile_options(heimdall PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-O3;${HEIMDALL_WIN_MARCH};${HEIMDALL_WIN_ASFLAGS}>")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    # this CPU's instructions and tuning (Pi 5: Cortex-A76) - see the Makefile
    target_compile_options(heimdall PRIVATE -O3 -mcpu=native)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    target_compile_options(heimdall PRIVATE -O3 -march=native -mtune=native)
else()
    target_compile_options(heimdall PRIVATE -O2)
endif()
# sqrt & co. without an errno path, complex * / without the C99 NaN-recovery
# call: both let loops vectorize (see the Makefile's CFLAGS)
target_compile_options(heimdall PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-math-errno;-fcx-fortran-rules>")

# Find uSockets object files
file(GLOB USOCKETS_OBJS "${CMAKE_CURRENT_SOURCE_DIR}/uWebSockets/uSockets/*.o")

# Link libraries
if(WIN32)
    # One self-contained exe (no MSYS2 DLLs needed to run it) - static
    # libstdc++ / winpthread / libusb / libuv / OpenSSL / zlib, plus the
    # Windows system libraries they use (avrt: MMCSS thread priority;
    # setupapi + hid: the moRFeus LO of the Wideband variant)
    target_link_libraries(heimdall
        -static
        ${RTLSDR_STATIC_LIB}
        ${LIBUSB_STATIC_LIBRARIES}
        pffft
        ${USOCKETS_OBJS}
        ${LIBUV_STATIC_LIBRARIES}
        Threads::Threads
        Eigen3::Eigen
        ssl crypto z
        ws2_32 crypt32 bcrypt gdi32 user32 advapi32 winmm avrt setupapi hid
    )
    target_link_directories(heimdall PRIVATE
        ${LIBUSB_STATIC_LIBRARY_DIRS}
        ${LIBUV_STATIC_LIBRARY_DIRS}
    )
else()
target_link_libraries(heimdall
    ${RTLSDR_STATIC_LIB}
    ${LIBUSB_LIBRARIES}
    pffft
    ${USOCKETS_OBJS}
    Threads::Threads
    Eigen3::Eigen
    m
    z
    ssl
    crypto
)

# Link directories
target_link_directories(heimdall PRIVATE
    ${LIBUSB_LIBRARY_DIRS}
)
endif()

# Installation
install(TARGETS heimdall DESTINATION bin)

# config.h is the single source of configuration (checked into the repo).
# It used to be auto-generated here with values that had drifted from the
# real file - never regenerate it, just require it.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.h")
    message(FATAL_ERROR "config.h not found in ${CMAKE_CURRENT_SOURCE_DIR} - it ships with the repo; restore it from git.")
endif()

# Custom targets
add_custom_target(run
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/heimdall
    DEPENDS heimdall
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running Heimdall with eigenvalue decomposition..."
)

# Status target
add_custom_target(status
    COMMAND ${CMAKE_COMMAND} -E echo "=== Build Status ==="
    COMMAND ${CMAKE_COMMAND} -E echo "Eigen3 found: ${EIGEN3_FOUND}"
    COMMAND ${CMAKE_COMMAND} -E echo "Eigen3 version: ${EIGEN3_VERSION}"
    COMMAND ${CMAKE_COMMAND} -E echo "RTL-SDR: KrakenSDR fork, static (${RTLSDR_STATIC_LIB})"
    COMMAND ${CMAKE_COMMAND} -E echo "FFT: pffft (vendored, external/pffft)"
    COMMENT "Checking build status..."
)

# Display configuration summary
message(STATUS "")
message(STATUS "=== Heimdall Configuration ===")
message(STATUS "C++ Standard: C++20")
message(STATUS "Eigen3: ${EIGEN3_VERSION} (${EIGEN3_INCLUDE_DIR})")
message(STATUS "RTL-SDR: KrakenSDR fork, static (${RTLSDR_STATIC_LIB})")
message(STATUS "FFT: pffft (vendored, external/pffft)")
message(STATUS "Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Phase Calibration: Eigenvalue Decomposition")
message(STATUS "==============================")
message(STATUS "")