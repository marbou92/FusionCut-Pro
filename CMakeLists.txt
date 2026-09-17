cmake_minimum_required(VERSION 3.16)

# Single source of truth for the version number.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FC_RAW_VERSION)
string(STRIP "${FC_RAW_VERSION}" FC_VERSION)

project(FusionCutPro
    VERSION ${FC_VERSION}
    DESCRIPTION "FusionCut Pro - lightweight dual-mode video editor (Windows 7+, 1GB RAM target)"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FC_BUILD_TESTS "Build unit tests" ON)
option(FC_BUILD_APP "Build the Qt desktop application" OFF)
option(FC_BUILD_MEDIA "Build the FFmpeg media I/O layer (requires pkg-config + FFmpeg dev)" ON)

if(FC_BUILD_APP AND NOT FC_BUILD_MEDIA)
    message(FATAL_ERROR "FC_BUILD_APP=ON requires FC_BUILD_MEDIA=ON (the shell "
                        "links fc_media for decode/proxy playback).")
endif()

if(FC_BUILD_MEDIA)
    # Discovered at the top level so the PkgConfig::FFMPEG IMPORTED target is
    # visible in every subdirectory (imported targets are directory-scoped:
    # creating it inside src/media would hide it from tests/).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale
        libswresample)
endif()

# Windows API surface gate. Without it the MinGW/MSVC headers default to
# _WIN32_WINNT=0x0A00 (Windows 10), so a Win8/10-only declaration would
# compile in locally and only fail on the Windows 7 runtime the project
# targets - the Win7 gate must exist at compile time too, not just as the
# runtime tripwires (import blocklist, api-set shim). 0x0601 = Windows 7,
# the oldest supported OS, hides later declarations at compile time.
# WINVER follows _WIN32_WINNT on current MinGW/MSVC headers, but is pinned
# as well so header revisions that treat the two independently cannot
# drift. Applies to every target below (core, media, app, tests).
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
endif()

# MinGW portable build: link the C/C++ runtimes statically so the shipped
# exe does not need libgcc/libstdc++ DLLs beside it (the portable tree
# carries only Qt + FFmpeg). Directory-wide, so the test executables the
# MSYS2 media-tests CI job builds get it too - static libgcc/libstdc++ is
# safe and standard for MinGW binaries, and the FFmpeg DLLs they load are
# resolved from PATH exactly as before.
if(MINGW)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

# Warning flags for the dependency-free core and its tests: fully ours to
# fix, and the -Wall -Wextra -Wpedantic -Wshadow set has already earned
# its keep (the dead srt helper surfaced here). The media/app layers
# compile FFmpeg's and Qt's headers and stay on the toolchain defaults so
# third-party noise does not bury our warnings in CI logs. Not an error
# set: new compiler versions may add diagnostics and the build must not
# break for that alone.
add_library(fc_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fc_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
elseif(MSVC)
    target_compile_options(fc_warnings INTERFACE /W4)
endif()

# Generated headers (version information).
configure_file(cmake/version.h.in generated/fc/version.h @ONLY)

enable_testing()

add_subdirectory(src/core)
if(FC_BUILD_MEDIA)
    add_subdirectory(src/media)
endif()
if(FC_BUILD_APP)
    add_subdirectory(src/app)
endif()
if(FC_BUILD_TESTS)
    add_subdirectory(tests)
endif()
