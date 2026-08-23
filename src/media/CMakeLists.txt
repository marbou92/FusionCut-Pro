# fc_media: FFmpeg-backed media I/O layer (probe / decode / proxy).
# PkgConfig::FFMPEG is discovered once in the top-level CMakeLists (see the
# directory-scoping note there) and is visible here.

add_library(fc_media STATIC
    ffmpeg_wrappers.cpp
    media_probe.cpp
    video_decoder.cpp
    proxy_generator.cpp)

target_include_directories(fc_media PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fc_media PUBLIC
    fc_core
    PkgConfig::FFMPEG)
