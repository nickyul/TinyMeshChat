#include "tmc/app/application_controller.h"
#include "tmc/cli/console_controller.h"
#include "tmc/ui/app_link_controller.h"
#include "tmc/ui/app_view_model.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

struct CommandLine {
    bool console{false};
    bool qmlSmoke{false};
    QString displayName;
    QUrl appLink;
    QString error;
};

CommandLine parseCommandLine(int argc, char** argv) {
    CommandLine options;
    for (int i = 1; i < argc; ++i) {
        const auto argument = QString::fromLocal8Bit(argv[i]);
        if (argument == "--console") {
            options.console = true;
        } else if (argument == "--qml-smoke") {
            options.qmlSmoke = true;
        } else if (argument == "--display-name") {
            if (i + 1 >= argc) {
                options.error = "--display-name requires a value";
                break;
            }
            options.displayName = QString::fromLocal8Bit(argv[++i]);
        } else if (argument.startsWith("tinymesh://")) {
            options.appLink = QUrl(argument);
        }
    }
    return options;
}

void configureApplication(QCoreApplication& app) {
    app.setApplicationName("TinyMesh Chat");
    app.setOrganizationName("TinyMesh");
}

void prepareConsole() {
#ifdef Q_OS_WIN
    const auto inputHandle = GetStdHandle(STD_INPUT_HANDLE);
    const auto outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto errorHandle = GetStdHandle(STD_ERROR_HANDLE);
    const auto isRedirected = [](HANDLE handle) {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
               GetFileType(handle) != FILE_TYPE_CHAR;
    };

    const auto inputRedirected = isRedirected(inputHandle);
    const auto outputRedirected = isRedirected(outputHandle);
    const auto errorRedirected = isRedirected(errorHandle);

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        const auto error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && !AllocConsole()) {
            return;
        }
    }

    FILE* stream{};
    if (!inputRedirected) {
        freopen_s(&stream, "CONIN$", "r", stdin);
    }
    if (!outputRedirected) {
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    if (!errorRedirected) {
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parseCommandLine(argc, argv);
    if (options.console) {
        prepareConsole();
        QCoreApplication app(argc, argv);
        configureApplication(app);
        if (!options.error.isEmpty()) {
            fprintf(stderr, "%s\n", options.error.toUtf8().constData());
            return 2;
        }
        tmc::ApplicationController controller;
        const auto initialized = controller.initialize();
        if (!initialized) {
            fprintf(stderr, "%s\n", initialized.error().toUtf8().constData());
            return 1;
        }
        if (initialized.value() == tmc::InitializationState::DisplayNameRequired) {
            if (options.displayName.trimmed().isEmpty()) {
                fprintf(stderr, "First launch requires --display-name <name> in console mode.\n");
                return 2;
            }
            const auto created = controller.createIdentity(options.displayName);
            if (!created) {
                fprintf(stderr, "%s\n", created.error().toUtf8().constData());
                return 1;
            }
        }
        tmc::ConsoleController console(controller);
        return console.run();
    }

    QGuiApplication app(argc, argv);
    configureApplication(app);
    QQuickStyle::setStyle("Basic");
    if (!options.error.isEmpty()) {
        fprintf(stderr, "%s\n", options.error.toUtf8().constData());
        return 2;
    }

    tmc::AppLinkController appLinks;
    if (!appLinks.startPrimary(options.appLink)) {
        return 0;
    }

    tmc::ApplicationController controller;
    const auto initialized = controller.initialize();
    if (!initialized) {
        fprintf(stderr, "%s\n", initialized.error().toUtf8().constData());
        return 1;
    }

    bool identityRequired = initialized.value() == tmc::InitializationState::DisplayNameRequired;
    if (identityRequired && !options.displayName.trimmed().isEmpty()) {
        const auto created = controller.createIdentity(options.displayName);
        if (!created) {
            fprintf(stderr, "%s\n", created.error().toUtf8().constData());
            return 1;
        }
        identityRequired = false;
    }

    tmc::AppViewModel viewModel(controller, appLinks, identityRequired);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("appViewModel", &viewModel);
    engine.rootContext()->setContextProperty("qmlSmoke", options.qmlSmoke);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    const auto activate = [&engine] {
        if (const auto root = qobject_cast<QWindow*>(engine.rootObjects().constFirst())) {
            root->show();
            root->raise();
            root->requestActivate();
        }
    };
    QObject::connect(&appLinks, &tmc::AppLinkController::urlReceived, &viewModel,
                     [&viewModel, activate](const QUrl& url) {
                         activate();
                         viewModel.previewSignalingLink(url.toString(QUrl::FullyEncoded));
                     });
    QObject::connect(&appLinks, &tmc::AppLinkController::activationRequested, &viewModel, activate);
    if (options.appLink.isValid()) {
        QTimer::singleShot(0, &viewModel, [&viewModel, url = options.appLink] {
            viewModel.previewSignalingLink(url.toString(QUrl::FullyEncoded));
        });
    }
    if (options.qmlSmoke) {
        if (!identityRequired) {
            viewModel.createMesh();
        }
        if (!identityRequired) {
            viewModel.sendMessage("QML startup smoke");
        }
        QTimer::singleShot(100, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
