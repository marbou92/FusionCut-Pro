#include <QApplication>
#include <QStringList>

#include <fc/version.h>

#include "crash_handler.h"
#include "mainwindow.h"

int main(int argc, char *argv[]) {
    // Install process-wide crash diagnostics BEFORE QApplication so the
    // handler covers the qwindows.dll platform-plugin init path (the
    // historic 0xc0000005 startup mode). On any unhandled exception,
    // signal, or CRT misuse it writes FusionCutPro-crash-<ts>.log next
    // to the executable and (Windows) shows a MessageBox naming the log.
    // Replaces the run-console.bat diagnostic launcher.
    fc::installCrashHandler(FC_VERSION_STRING);

    // Manual smoke-test hook for end-to-end verification on a clean
    // Windows machine: `FusionCutPro.exe --crash-test` writes a
    // synthetic report and exits without starting the UI. Lets the
    // user confirm the crash-log path + dialog behave as expected.
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--crash-test")) {
            const std::string path = fc::writeManualCrashReport(FC_VERSION_STRING);
            std::fprintf(stdout, "FusionCut Pro crash-test report written to: %s\n",
                         path.empty() ? "(failed)" : path.c_str());
            return 0;
        }
    }

    // Module 10.4 UI scaling support (100-200%) on high-DPI displays.
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    QApplication::setApplicationName(FC_APP_NAME);
    QApplication::setOrganizationName("FusionCut");
    QApplication::setApplicationVersion(FC_VERSION_STRING);

    fc::MainWindow window;
    window.show();
    return app.exec();
}
