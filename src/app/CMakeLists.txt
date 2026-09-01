set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.12 COMPONENTS Widgets REQUIRED)

add_executable(FusionCutPro WIN32
    main.cpp
    mainwindow.cpp
    mainwindow.h
    crash_handler.cpp
    crash_handler.h
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

# Crash handler: MessageBoxA + Toolhelp32 (module snapshot) live in
# user32 / kernel32 respectively. kernel32 is auto-linked on Windows;
# user32 is NOT for a WIN32 exe under MinGW/MSVC unless we ask. Guarded
# so the Linux Qt compile path (the qt-app-ubuntu CI leg) is unaffected.
if(WIN32)
    target_link_libraries(FusionCutPro PRIVATE user32)
endif()

# Portable layout: the executable and its dependencies all land in one
# flat directory; CI runs windeployqt plus an ldd sweep for FFmpeg/MinGW
# runtime DLLs during packaging.
install(TARGETS FusionCutPro RUNTIME DESTINATION .)

# ---------------------------------------------------------------------------
# fcp-loader-check: zero-dependency Windows loader diagnostic.
#
# The Windows loader dialog "The application was unable to start
# correctly (0xc0000005)" is shown BEFORE any user code in
# FusionCutPro.exe runs (it is a loader-phase failure during
# import-resolution / DllMain / CRT-init). The in-process crash
# handler installed at .CRT$XCU static-init time is therefore never
# installed for this crash class - no log is produced. This tool
# walks the PE import tree of FusionCutPro.exe from OUTSIDE the
# process and names any DLL the loader cannot map, which is the
# diagnostic the bare "0xc0000005" dialog does not show.
#
# Design constraints:
#   * WIN32 subsystem app (no console flash on double-click).
#   * Fully static link (-static): zero DLL deps of its own, so it
#     runs even when the portable folder is missing a DLL that
#     breaks FusionCutPro.exe.
#   * Uses ONLY kernel32 + user32 (always present on every Windows
#     since NT 3.1).
#   * Walks imports via DONT_RESOLVE_DLL_REFERENCES - maps each image
#     without running DllMain, so a bad DllMain in a dependency
#     cannot crash the tool itself. Since v0.4.5 every probe mapping
#     is FreeLibrary'd right after its subtree walk (v0.4.4 left them
#     mapped and the loader teardown crashed the tool at exit).
#   * v0.4.5 phase 2: launches FusionCutPro.exe under this tool as
#     its DEBUGGER (DEBUG_ONLY_THIS_PROCESS + WaitForDebugEvent/
#     ContinueDebugEvent - kernel32 only, no dbghelp) and records the
#     module load trail + every exception with fault-address ->
#     module attribution. Debug events are delivered before Windows
#     Error Reporting, so this catches DllMain / static-initializer
#     faults that produce the "0xc0000005 unable to start" dialog
#     with no Event Viewer entry at all.
# Windows-only target; the qt-app-ubuntu CI leg does not build it.
# -------------------------------------------------------------------------
if(WIN32)
    add_executable(fcp-loader-check WIN32
        loader_check.cpp)

    target_link_libraries(fcp-loader-check PRIVATE user32)

    # Full static link so the tool has zero DLL dependencies of its
    # own. Without this, fcp-loader-check.exe would itself fail with
    # "0xc0000005 unable to start" if a MinGW runtime DLL was missing
    # - making it useless as a diagnostic. -static covers libgcc,
    # libstdc++, and libwinpthread.
    if(MINGW)
        target_link_options(fcp-loader-check PRIVATE -static)
    endif()

    install(TARGETS fcp-loader-check RUNTIME DESTINATION .)
endif()
