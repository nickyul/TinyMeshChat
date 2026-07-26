#include "tmc/bootstrap/gui_runtime.h"

#include "tmc/app/application_controller.h"
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
#include <utility>

namespace tmc {

GuiRuntime::GuiRuntime(int& argc, char** argv, ApplicationOptions options)
    : argc_(argc), argv_(argv), options_(std::move(options)) {
}

GuiRuntime::~GuiRuntime() = default;

int GuiRuntime::run() {
    application_ = std::make_unique<QGuiApplication>(argc_, argv_);
    application_->setApplicationName("TinyMesh Chat");
    application_->setOrganizationName("TinyMesh");
    QQuickStyle::setStyle("Basic");
    if (!options_.error.isEmpty()) {
        fprintf(stderr, "%s\n", options_.error.toUtf8().constData());
        return 2;
    }

    appLinks_ = std::make_unique<AppLinkController>();
    const auto instance = appLinks_->startPrimary(options_.appLink);
    if (!instance) {
        fprintf(stderr, "%s\n", instance.error().toUtf8().constData());
        return 1;
    }
    if (instance.value() == AppInstanceState::ForwardedToPrimary) {
        return 0;
    }

    controller_ = std::make_unique<ApplicationController>();
    const auto initialized = controller_->initialize();
    if (!initialized) {
        fprintf(stderr, "%s\n", initialized.error().toUtf8().constData());
        return 1;
    }

    bool identityRequired = initialized.value() == InitializationState::DisplayNameRequired;
    if (identityRequired && !options_.displayName.trimmed().isEmpty()) {
        const auto created = controller_->createIdentity(options_.displayName);
        if (!created) {
            fprintf(stderr, "%s\n", created.error().toUtf8().constData());
            return 1;
        }
        identityRequired = false;
    }

    viewModel_ =
        std::make_unique<AppViewModel>(*controller_, *appLinks_, identityRequired);
    engine_ = std::make_unique<QQmlApplicationEngine>();
    engine_->rootContext()->setContextProperty("appViewModel", viewModel_.get());
    engine_->rootContext()->setContextProperty("qmlSmoke", options_.qmlSmoke);
    engine_->load(QUrl("qrc:/qml/Main.qml"));
    if (engine_->rootObjects().isEmpty()) {
        return 1;
    }

    wireAppLinks();
    if (options_.appLink.isValid()) {
        QTimer::singleShot(0, viewModel_.get(), [this] {
            viewModel_->previewSignalingLink(
                options_.appLink.toString(QUrl::FullyEncoded));
        });
    }
    if (options_.qmlSmoke) {
        if (!identityRequired) {
            viewModel_->createMesh();
            viewModel_->sendMessage("QML startup smoke");
        }
        QTimer::singleShot(100, application_.get(), &QCoreApplication::quit);
    }
    return application_->exec();
}

void GuiRuntime::bringMainWindowToFront() {
    if (!engine_ || engine_->rootObjects().isEmpty()) {
        return;
    }
    if (const auto root = qobject_cast<QWindow*>(engine_->rootObjects().constFirst())) {
        root->show();
        root->raise();
        root->requestActivate();
    }
}

void GuiRuntime::wireAppLinks() {
    QObject::connect(appLinks_.get(), &AppLinkController::urlReceived, engine_.get(),
                     [this](const QUrl& url) {
                         bringMainWindowToFront();
                         viewModel_->previewSignalingLink(
                             url.toString(QUrl::FullyEncoded));
                     });
    QObject::connect(appLinks_.get(), &AppLinkController::activationRequested, engine_.get(),
                     [this] { bringMainWindowToFront(); });
}

} // namespace tmc
