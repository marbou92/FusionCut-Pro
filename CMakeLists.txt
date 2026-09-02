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
