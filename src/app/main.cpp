#include <QApplication>

#include <fc/version.h>

#include "mainwindow.h"

int main(int argc, char *argv[]) {
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
