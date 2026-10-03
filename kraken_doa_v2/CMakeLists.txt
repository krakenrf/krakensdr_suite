cmake_minimum_required(VERSION 3.16)
project(KrakenSDRDoA VERSION 1.0.0 LANGUAGES CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Include directories
include_directories(include)

# Find required packages
find_package(PkgConfig REQUIRED)

# Find FFTW3
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find Threads
find_package(Threads REQUIRED)

# Find Eigen3
find_package(Eigen3 REQUIRED)

# Find libuv (required for uSockets)
pkg_check_modules(LIBUV libuv)
if(NOT LIBUV_FOUND)
    message(STATUS "libuv not found, will install as dependency")
endif()

# Manually find liquid-dsp (usually not in cmake modules)
find_library(LIQUID_LIB liquid REQUIRED)
find_path(LIQUID_INCLUDE liquid/liquid.h)

if(NOT LIQUID_LIB)
    message(FATAL_ERROR "liquid-dsp library not found. Install with: sudo apt-get install libliquid-dev")
endif()

# Try to find uWebSockets first
find_path(UWS_INCLUDE uWS/App.h PATHS /usr/local/include /usr/include)
find_library(UWS_LIB uWS PATHS /usr/local/lib /usr/lib)

# If not found, build it automatically
if(NOT UWS_INCLUDE OR NOT UWS_LIB)
    message(STATUS "uWebSockets not found locally, downloading and building...")
    
    include(FetchContent)
    
    # Download uWebSockets
    FetchContent_Declare(
        uwebsockets
        GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
        GIT_TAG v20.62.0
        GIT_SHALLOW TRUE
    )
    
    FetchContent_MakeAvailable(uwebsockets)
    
    set(UWS_SOURCE_DIR ${uwebsockets_SOURCE_DIR})
    set(UWS_INCLUDE ${UWS_SOURCE_DIR}/src)
    
    # Build uSockets dependency (this is what actually needs to be compiled)
    file(GLOB_RECURSE USOCKETS_SOURCES 
        "${UWS_SOURCE_DIR}/uSockets/src/*.c"
    )
    
    # Filter out platform-specific files we don't need
    list(FILTER USOCKETS_SOURCES EXCLUDE REGEX ".*/(win32|darwin)/.*")
    
    # Debug: Print found sources
    message(STATUS "Found uSockets sources: ${USOCKETS_SOURCES}")
    
    if(NOT USOCKETS_SOURCES)
        message(FATAL_ERROR "No uSockets source files found in ${UWS_SOURCE_DIR}/uSockets/src/")
    endif()
    
    add_library(uSockets STATIC ${USOCKETS_SOURCES})
    
    # Explicitly set the language
    set_target_properties(uSockets PROPERTIES LINKER_LANGUAGE C)
    
    target_include_directories(uSockets PUBLIC 
        ${UWS_SOURCE_DIR}/uSockets/src
    )
    
    # Add required compile definitions for uSockets
    target_compile_definitions(uSockets PRIVATE
        LIBUS_USE_EPOLL=1
        LIBUS_USE_LIBUV=1
        LIBUS_USE_OPENSSL=1
    )
    
    # Link required libraries for uSockets
    if(LIBUV_FOUND)
        target_link_libraries(uSockets PRIVATE ${LIBUV_LIBRARIES})
        target_include_directories(uSockets PRIVATE ${LIBUV_INCLUDE_DIRS})
    endif()
    
    # uWebSockets is header-only, so we create an interface library
    add_library(uWS INTERFACE)
    
    target_include_directories(uWS INTERFACE 
        ${UWS_SOURCE_DIR}/src
        ${UWS_SOURCE_DIR}/uSockets/src
    )
    
    # Link uSockets to the interface library
    target_link_libraries(uWS INTERFACE uSockets)
    
    # Set variables for later use
    set(UWS_LIB uWS)
    
    message(STATUS "uWebSockets will be built from source")
else()
    message(STATUS "Found uWebSockets: ${UWS_LIB}")
endif()

# Source files
file(GLOB_RECURSE SIGNAL_PROCESSING_SOURCES "src/signal_processing/*.cpp")
file(GLOB_RECURSE NETWORKING_SOURCES "src/networking/*.cpp")
file(GLOB_RECURSE UTILS_SOURCES "src/utils/*.cpp")
file(GLOB_RECURSE DIGITAL_SOURCES "src/digital/*.cpp")
file(GLOB APP_SOURCES "src/*.cpp")

# All source files
set(ALL_SOURCES
    ${SIGNAL_PROCESSING_SOURCES}
    ${NETWORKING_SOURCES}
    ${DIGITAL_SOURCES}
    ${UTILS_SOURCES}
    ${APP_SOURCES}
)

# Create executable
add_executable(${PROJECT_NAME} ${ALL_SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${LIQUID_INCLUDE}
    ${UWS_INCLUDE}
    ${FFTW3F_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    ${FFTW3F_LIBRARIES}
    ${LIQUID_LIB}
    ${UWS_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    Eigen3::Eigen
    z
    sqlite3
    ${CMAKE_DL_LIBS}
)

# Compiler definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE ${FFTW3F_CFLAGS_OTHER})

# Install target
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Print configuration summary
message(STATUS "")
message(STATUS "KrakenSDR DoA Configuration Summary:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "Dependencies found:")
message(STATUS "  FFTW3F: ${FFTW3F_FOUND}")
message(STATUS "  OpenSSL: ${OPENSSL_FOUND}")
message(STATUS "  Eigen3: ${Eigen3_FOUND}")
message(STATUS "  liquid-dsp: ${LIQUID_LIB}")
message(STATUS "  uWebSockets: ${UWS_LIB}")
message(STATUS "")

# Custom target for dependencies
add_custom_target(deps
    COMMAND sudo apt-get update
    COMMAND sudo apt-get install -y build-essential cmake pkg-config
    COMMAND sudo apt-get install -y libfftw3-dev libliquid-dev
    COMMAND sudo apt-get install -y libeigen3-dev
    COMMAND sudo apt-get install -y libssl-dev libz-dev libuv1-dev
    COMMENT "Installing system dependencies (Ubuntu/Debian)"
)

# Custom target for uWebSockets installation
add_custom_target(install-uws
    COMMAND git clone --recursive https://github.com/uNetworking/uWebSockets /tmp/uWebSockets || true
    COMMAND cd /tmp/uWebSockets && make && sudo make install
    COMMENT "Installing uWebSockets manually"
)