cmake_minimum_required(VERSION 3.10)
project(heimdall)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Find Eigen3
find_package(Eigen3 3.3 REQUIRED NO_MODULE)

# Find other dependencies using pkg-config
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

# KrakenSDR librtlsdr fork, linked STATICALLY so a distro librtlsdr0 (earlier
# in the loader's search path than /usr/local/lib) can never shadow it - see
# the Makefile. Built by ../install.sh or by `make` in this directory.
set(RTLSDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../librtlsdr)
set(RTLSDR_STATIC_LIB ${RTLSDR_DIR}/build/src/librtlsdr.a)
if(NOT EXISTS "${RTLSDR_STATIC_LIB}")
    message(FATAL_ERROR "KrakenSDR librtlsdr fork not built (${RTLSDR_STATIC_LIB}). "
                        "Run ../install.sh, or `make ../librtlsdr/build/src/librtlsdr.a` in heimdall_v2.")
endif()

# Check for uWebSockets
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/uWebSockets")
    message(FATAL_ERROR "uWebSockets not found. Please run: git clone --recursive https://github.com/uNetworking/uWebSockets && cd uWebSockets/uSockets && make")
endif()

# Source files
set(CORE_SOURCES
    src/core/utils.cpp
    src/core/settings.cpp
    src/core/forward_comp.cpp
    src/core/status_dashboard.cpp
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
    ${FFTW3F_INCLUDE_DIRS}
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
    ${FFTW3F_CFLAGS_OTHER}
)

# Architecture-specific optimizations
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_compile_options(heimdall PRIVATE -O3 -march=armv8-a -mtune=cortex-a72 -ftree-vectorize)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    target_compile_options(heimdall PRIVATE -O3 -march=native -mtune=native)
else()
    target_compile_options(heimdall PRIVATE -O2)
endif()

# Find uSockets object files
file(GLOB USOCKETS_OBJS "${CMAKE_CURRENT_SOURCE_DIR}/uWebSockets/uSockets/*.o")

# Link libraries
target_link_libraries(heimdall
    ${RTLSDR_STATIC_LIB}
    ${LIBUSB_LIBRARIES}
    ${FFTW3F_LIBRARIES}
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
    ${FFTW3F_LIBRARY_DIRS}
)

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
    COMMAND ${CMAKE_COMMAND} -E echo "FFTW3 found: ${FFTW3F_FOUND}"
    COMMENT "Checking build status..."
)

# Display configuration summary
message(STATUS "")
message(STATUS "=== Heimdall Configuration ===")
message(STATUS "C++ Standard: C++20")
message(STATUS "Eigen3: ${EIGEN3_VERSION} (${EIGEN3_INCLUDE_DIR})")
message(STATUS "RTL-SDR: KrakenSDR fork, static (${RTLSDR_STATIC_LIB})")
message(STATUS "FFTW3: ${FFTW3F_VERSION}")
message(STATUS "Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Phase Calibration: Eigenvalue Decomposition")
message(STATUS "==============================")
message(STATUS "")