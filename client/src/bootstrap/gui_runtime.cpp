#include "tmc/bootstrap/gui_runtime.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/update_service.h"
#include "tmc/ui/app_link_controller.h"
#include "tmc/ui/app_view_model.h"
#include "tmc/ui/tray_controller.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
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
    application_ = std::make_unique<QApplication>(argc_, argv_);
    application_->setApplicationName("TinyMesh Chat");
    application_->setOrganizationName("TinyMesh");
    application_->setApplicationVersion(QStringLiteral(TMC_APP_VERSION));
    application_->setWindowIcon(QIcon(QStringLiteral(":/icons/tinymesh.png")));
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

    updates_ = std::make_unique<UpdateService>();
    viewModel_ = std::make_unique<AppViewModel>(*controller_, *appLinks_, *updates_,
                                                identityRequired);
    engine_ = std::make_unique<QQmlApplicationEngine>();
    engine_->rootContext()->setContextProperty("appViewModel", viewModel_.get());
    engine_->rootContext()->setContextProperty("qmlSmoke", options_.qmlSmoke);
    engine_->load(QUrl("qrc:/qml/Main.qml"));
    if (engine_->rootObjects().isEmpty()) {
        return 1;
    }

    wireAppLinks();
    QObject::connect(updates_.get(), &UpdateService::restartRequested, application_.get(),
                     [this] {
                         if (tray_) {
                             tray_->quitApplication();
                         } else {
                             if (viewModel_ && viewModel_->meshVisible()) {
                                 viewModel_->leaveMesh();
                             }
                             QCoreApplication::quit();
                         }
                     });
    if (options_.appLink.isValid()) {
        QTimer::singleShot(0, viewModel_.get(), [this] {
            viewModel_->importSignalingText(options_.appLink.toString(QUrl::FullyEncoded));
        });
    }
    if (options_.qmlSmoke) {
        if (!identityRequired) {
            viewModel_->createMesh();
            viewModel_->sendMessage("QML startup smoke");
        }
        QTimer::singleShot(100, application_.get(), &QCoreApplication::quit);
    } else {
        if (auto* root = qobject_cast<QWindow*>(engine_->rootObjects().constFirst())) {
            tray_ = std::make_unique<TrayController>(*root, *viewModel_);
        }
        QTimer::singleShot(0, viewModel_.get(),
                           &AppViewModel::checkForUpdatesAutomatically);
    }
    return application_->exec();
}

void GuiRuntime::bringMainWindowToFront() {
    if (!engine_ || engine_->rootObjects().isEmpty()) {
        return;
    }
    if (const auto root = qobject_cast<QWindow*>(engine_->rootObjects().constFirst())) {
        if (root->windowState() == Qt::WindowMinimized) {
            root->showNormal();
        } else {
            root->show();
        }
        root->raise();
        root->requestActivate();
    }
}

void GuiRuntime::wireAppLinks() {
    QObject::connect(appLinks_.get(), &AppLinkController::urlReceived, engine_.get(),
                      [this](const QUrl& url) {
                          bringMainWindowToFront();
                          viewModel_->importSignalingText(url.toString(QUrl::FullyEncoded));
                      });
    QObject::connect(appLinks_.get(), &AppLinkController::activationRequested, engine_.get(),
                     [this] { bringMainWindowToFront(); });
}

} // namespace tmc
