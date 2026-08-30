add_executable(fc_core_tests test_main.cpp)
target_link_libraries(fc_core_tests PRIVATE fc_core)
add_test(NAME core COMMAND fc_core_tests)

add_executable(fc_timeline_tests timeline_tests.cpp)
target_link_libraries(fc_timeline_tests PRIVATE fc_core)
add_test(NAME timeline COMMAND fc_timeline_tests)

if(TARGET fc_media)
    add_executable(fc_media_tests
        media_tests.cpp
        media_test_util.cpp)
    target_link_libraries(fc_media_tests PRIVATE
        fc_media
        PkgConfig::FFMPEG)
    add_test(NAME media COMMAND fc_media_tests)
endif()
