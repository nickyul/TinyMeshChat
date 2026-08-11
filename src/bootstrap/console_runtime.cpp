#include "tmc/bootstrap/console_runtime.h"

#include "tmc/app/application_controller.h"
#include "tmc/cli/console_controller.h"

#include <QCoreApplication>

#include <cstdio>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace tmc {

ConsoleRuntime::ConsoleRuntime(int& argc, char** argv, ApplicationOptions options)
    : argc_(argc), argv_(argv), options_(std::move(options)) {
}

void ConsoleRuntime::prepareNativeConsole() {
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

int ConsoleRuntime::run() {
    prepareNativeConsole();

    QCoreApplication application(argc_, argv_);
    application.setApplicationName("TinyMesh Chat");
    application.setOrganizationName("TinyMesh");
    application.setApplicationVersion(QStringLiteral(TMC_APP_VERSION));
    if (!options_.error.isEmpty()) {
        fprintf(stderr, "%s\n", options_.error.toUtf8().constData());
        return 2;
    }

    ApplicationController controller;
    const auto initialized = controller.initialize();
    if (!initialized) {
        fprintf(stderr, "%s\n", initialized.error().toUtf8().constData());
        return 1;
    }
    if (initialized.value() == InitializationState::DisplayNameRequired) {
        if (options_.displayName.trimmed().isEmpty()) {
            fprintf(stderr, "First launch requires --display-name <name> in console mode.\n");
            return 2;
        }
        const auto created = controller.createIdentity(options_.displayName);
        if (!created) {
            fprintf(stderr, "%s\n", created.error().toUtf8().constData());
            return 1;
        }
    }

    ConsoleController console(controller);
    return console.run();
}

} // namespace tmc
