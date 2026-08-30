#include <QApplication>
#include <QStringList>

#include <fc/version.h>

#include "crash_handler.h"
#include "mainwindow.h"

int main(int argc, char *argv[]) {
    // The crash handler is *already installed* by the file-scope static
    // initializer in crash_handler.cpp (which ran at .CRT$XCU static-init
    // time, before this point). What we do here is supply the real
    // FC_VERSION_STRING (the static initializer used "unknown") and refresh
    // the report dir. The idempotent guard in installCrashHandler keeps
    // the VEH + signal handlers in place; only the version + dir fields
    // are updated. The boot trace records stage1 here.
    fc::installCrashHandler(FC_VERSION_STRING);
    fc::recordBootStage(2, "entered main()");

    // Manual smoke-test hook for end-to-end verification on a clean
    // Windows machine: `FusionCutPro.exe --crash-test` writes a
    // synthetic report and exits without starting the UI. Lets the
    // user confirm the crash-log path + dialog behave as expected.
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--crash-test")) {
            fc::recordBootStage(3, "--crash-test invoked, writing manual report");
            const std::string path = fc::writeManualCrashReport(FC_VERSION_STRING);
            std::fprintf(stdout, "FusionCut Pro crash-test report written to: %s\n",
                         path.empty() ? "(failed)" : path.c_str());
            // Leave the boot trace in place (do NOT call
            // shutdownCrashHandler) so the developer can see the
            // stage0/stage1/stage2/stage3 trail alongside the manual
            // crash report - useful for diagnosing the report path
            // resolution itself if the --crash-test failed.
            return 0;
        }
    }

    // Module 10.4 UI scaling support (100-200%) on high-DPI displays.
    fc::recordBootStage(4, "about to construct QApplication");
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    QApplication::setApplicationName(FC_APP_NAME);
    QApplication::setOrganizationName("FusionCut");
    QApplication::setApplicationVersion(FC_VERSION_STRING);
    fc::recordBootStage(5, "QApplication constructed");

    fc::MainWindow window;
    fc::recordBootStage(6, "MainWindow constructed");

    window.show();
    fc::recordBootStage(7, "window.show() returned, entering app.exec()");

    const int code = app.exec();
    // On clean exit, mark the boot trace for deletion so it doesn't
    // accumulate across runs. On crash we never reach this line, so
    // the boot trace file survives and (additionally) gets embedded
    // into the crash report body by the VEH/terminate handlers.
    fc::shutdownCrashHandler();
    return code;
}
