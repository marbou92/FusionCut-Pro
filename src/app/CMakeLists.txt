set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.12 COMPONENTS Widgets REQUIRED)

add_executable(FusionCutPro WIN32
    main.cpp
    mainwindow.cpp
    mainwindow.h
    decode_worker.cpp
    decode_worker.h
    preview_canvas.cpp
    preview_canvas.h
    transport_bar.cpp
    transport_bar.h
    project_panel.cpp
    project_panel.h
    effects_panel.cpp
    effects_panel.h
    timeline_panel.cpp
    timeline_panel.h
    mixer_panel.cpp
    mixer_panel.h
    effect_controls_panel.cpp
    effect_controls_panel.h
    quick_mode_view.cpp
    quick_mode_view.h)

target_include_directories(FusionCutPro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(FusionCutPro PRIVATE
    fc_core
    fc_media
    Qt5::Widgets)

# Portable layout: the executable and its dependencies all land in one
# flat directory; CI runs windeployqt plus an ldd sweep for FFmpeg/MinGW
# runtime DLLs during packaging.
install(TARGETS FusionCutPro RUNTIME DESTINATION .)
