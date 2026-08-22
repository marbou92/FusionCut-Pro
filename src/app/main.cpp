#include <QApplication>

#include <fc/version.h>

#include "mainwindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(FC_APP_NAME);
    QApplication::setOrganizationName("FusionCut");
    QApplication::setApplicationVersion(FC_VERSION_STRING);

    fc::MainWindow window;
    window.show();
    return app.exec();
}
