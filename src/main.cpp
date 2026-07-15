#include "app/application_controller.h"
#include "cli/console_controller.h"
#include "ui/main_window.h"
#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
int main(int argc, char** argv) {
    bool console = false;
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == "--console")
            console = true;
    if (console) {
        QCoreApplication app(argc, argv);
        app.setApplicationName("TinyMesh Chat");
        app.setOrganizationName("TinyMesh");
        tmc::ApplicationController controller;
        auto init = controller.initialize(qEnvironmentVariable("USERNAME", "User"));
        if (!init) {
            fprintf(stderr, "%s\n", init.error().toUtf8().constData());
            return 1;
        }
        tmc::ConsoleController cli(controller);
        return cli.run();
    }
    QApplication app(argc, argv);
    app.setApplicationName("TinyMesh Chat");
    app.setOrganizationName("TinyMesh");
    tmc::ApplicationController controller;
    auto init = controller.initialize(qEnvironmentVariable("USERNAME", "User"));
    if (!init) {
        QMessageBox::critical(nullptr, "TinyMesh Chat", init.error());
        return 1;
    }
    tmc::MainWindow window(controller);
    window.show();
    return app.exec();
}
