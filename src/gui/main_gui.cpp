#include <QApplication>
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("SoftDips Manager");
    app.setApplicationVersion("0.1.1");
    app.setOrganizationName("morbware");

    MainWindow window;
    window.show();

    // If a path was passed as argument, open it (file or directory).
    if (argc > 1) {
        window.openPath(argv[1]);
    }

    return app.exec();
}