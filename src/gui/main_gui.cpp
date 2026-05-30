#include <QApplication>
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("SoftDips Manager");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("morbware");

    MainWindow window;
    window.show();

    // If a file was passed as argument, open it
    if (argc > 1) {
        // TODO: Open file from command line argument
    }

    return app.exec();
}