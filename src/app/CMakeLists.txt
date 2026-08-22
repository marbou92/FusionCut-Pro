set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.12 COMPONENTS Widgets REQUIRED)

add_executable(FusionCutPro WIN32
    main.cpp
    mainwindow.cpp
    mainwindow.h)

target_include_directories(FusionCutPro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(FusionCutPro PRIVATE
    fc_core
    Qt5::Widgets)

# Portable layout: the executable and its dependencies all land in one
# flat directory (populated by windeployqt during packaging).
install(TARGETS FusionCutPro RUNTIME DESTINATION .)
