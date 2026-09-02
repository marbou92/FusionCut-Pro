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

# ---------------------------------------------------------------------------
# fcp-apiset-synch: api-ms-win-core-synch-l1-2-0.dll compatibility shim.
#
# A real, small DLL (built from api_set_synch.c - see its header for the
# full root-cause narrative) that provides the WaitOnAddress futex API
# set for the Windows 7 / 8.x loaders. librav1e.dll (Rust, in MSYS2's
# FFmpeg 8 dependency tree) hard-imports that api set by literal name;
# pre-10 loaders cannot resolve it and FusionCutPro.exe dies with the
# bare "unable to start correctly (0xc0000005)" dialog before any user
# code runs. The DLL search order tries the application directory
# before System32, so installing this DLL next to FusionCutPro.exe
# wins the bind on Win7/8.x; on Win10/11 the ApiSetSchema resolves the
# name first and this DLL is never touched (inert).
#
# Why real implementations instead of v0.4.6's forwarder .def: KernelBase
# only implements the futex family since Windows 8, so a forwarder would
# fail entry-point resolution on Windows 7 (the user's test platform)
# and trade the 0xc0000005 startup abort for a "procedure entry point
# not located" abort. api_set_synch.c implements the semantics with
# Win7-era kernel32 primitives only (SRWLOCK + CONDITION_VARIABLE).
#
# Build contract (mirrors the zero-dependency rule of fcp-loader-check):
#   * OUTPUT_NAME + empty PREFIX give the EXACT api-set file name - the
#     name IS the fix; a lib-prefixed name would never be found.
#   * -nostdlib: no CRT, no MinGW runtime - kernel32 is the ONLY import.
#     The source follows CRT-free code rules (see its header), so GCC
#     never needs memset/memcpy/libgcc helpers.
#   * --entry=DllMain: with -nostdlib ld cannot find the default
#     DllMainCRTStartup entry (the v0.4.6 CI run printed that warning);
#     our own DllMain is the entry point.
# Windows-only target; the qt-app-ubuntu CI leg does not build it.
# -------------------------------------------------------------------------
if(WIN32)
    add_library(fcp-apiset-synch SHARED api_set_synch.c)

    set_target_properties(fcp-apiset-synch PROPERTIES
        AUTOMOC OFF
        OUTPUT_NAME "api-ms-win-core-synch-l1-2-0"
        PREFIX "")

    target_link_libraries(fcp-apiset-synch PRIVATE kernel32)

    if(MINGW)
        target_link_options(fcp-apiset-synch PRIVATE
            -nostdlib -Wl,--entry=DllMain)
    endif()

    install(TARGETS fcp-apiset-synch RUNTIME DESTINATION .)
endif()
