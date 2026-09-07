add_executable(fc_core_tests test_main.cpp)
target_link_libraries(fc_core_tests PRIVATE fc_core)
add_test(NAME core COMMAND fc_core_tests)

add_executable(fc_timeline_tests timeline_tests.cpp)
target_link_libraries(fc_timeline_tests PRIVATE fc_core)
add_test(NAME timeline COMMAND fc_timeline_tests)

add_executable(fc_effect_tests effect_tests.cpp)
target_link_libraries(fc_effect_tests PRIVATE fc_core)
add_test(NAME effects COMMAND fc_effect_tests)

add_executable(fc_transition_tests transition_tests.cpp)
target_link_libraries(fc_transition_tests PRIVATE fc_core)
add_test(NAME transitions COMMAND fc_transition_tests)

add_executable(fc_project_tests project_format_tests.cpp)
target_link_libraries(fc_project_tests PRIVATE fc_core)
add_test(NAME project COMMAND fc_project_tests)

add_executable(fc_text_tests text_tests.cpp)
target_link_libraries(fc_text_tests PRIVATE fc_core)
add_test(NAME text COMMAND fc_text_tests)

# The emoji suite pins its expectations against the COMMITTED font
# bytes, so the source-tree path is baked in at configure time (the
# checkout always contains it; a missing file fails the suite loudly).
add_executable(fc_emoji_tests emoji_tests.cpp)
target_link_libraries(fc_emoji_tests PRIVATE fc_core)
target_compile_definitions(fc_emoji_tests PRIVATE
    FC_EMOJI_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../resources/fonts/NotoColorEmoji.ttf")
add_test(NAME emoji COMMAND fc_emoji_tests)

if(TARGET fc_media)
    add_executable(fc_media_tests
        media_tests.cpp
        media_test_util.cpp)
    target_link_libraries(fc_media_tests PRIVATE
        fc_media
        PkgConfig::FFMPEG)
    add_test(NAME media COMMAND fc_media_tests)
endif()
