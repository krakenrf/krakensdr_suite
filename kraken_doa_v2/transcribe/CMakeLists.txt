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
