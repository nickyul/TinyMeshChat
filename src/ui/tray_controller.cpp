#include "tmc/ui/tray_controller.h"

#include "tmc/ui/app_view_model.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QWindow>

namespace tmc {

TrayController::TrayController(QWindow& window, AppViewModel& viewModel, QObject* parent)
    : QObject(parent), window_(window), viewModel_(viewModel), menu_(std::make_unique<QMenu>()),
      trayIcon_(std::make_unique<QSystemTrayIcon>()) {
    auto icon = QIcon::fromTheme("network-workgroup");
    if (icon.isNull()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }

    trayIcon_->setIcon(icon);
    trayIcon_->setToolTip(tr("TinyMesh Chat"));

    auto* openAction = menu_->addAction(tr("Открыть TinyMesh Chat"));
    muteAction_ = menu_->addAction(tr("Выключить микрофон"));
    leaveMeshAction_ = menu_->addAction(tr("Выйти из mesh"));
    menu_->addSeparator();
    auto* quitAction = menu_->addAction(tr("Завершить TinyMesh Chat"));
    trayIcon_->setContextMenu(menu_.get());

    connect(openAction, &QAction::triggered, this, &TrayController::restoreWindow);
    connect(muteAction_, &QAction::triggered, &viewModel_, &AppViewModel::toggleMute);
    connect(leaveMeshAction_, &QAction::triggered, &viewModel_, &AppViewModel::leaveMesh);
    connect(quitAction, &QAction::triggered, this, &TrayController::quitApplication);
    connect(trayIcon_.get(), &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    restoreWindow();
                }
            });
    connect(&viewModel_, &AppViewModel::meshStateChanged, this,
            &TrayController::updateTrayState);
    connect(&viewModel_, &AppViewModel::callStateChanged, this,
            &TrayController::updateTrayState);

    window_.installEventFilter(this);
    updateTrayState();
}

TrayController::~TrayController() {
    window_.removeEventFilter(this);
}

void TrayController::quitApplication() {
    if (quitting_) {
        return;
    }
    quitting_ = true;
    trayIcon_->hide();
    if (viewModel_.meshVisible()) {
        viewModel_.leaveMesh();
    }
    QCoreApplication::quit();
}

bool TrayController::eventFilter(QObject* watched, QEvent* event) {
    if (watched != &window_ || event->type() != QEvent::Close || quitting_ ||
        !viewModel_.meshVisible() || !QSystemTrayIcon::isSystemTrayAvailable()) {
        return QObject::eventFilter(watched, event);
    }

    static_cast<QCloseEvent*>(event)->ignore();
    window_.hide();
    return true;
}

void TrayController::restoreWindow() {
    if (window_.windowState() == Qt::WindowMinimized) {
        window_.showNormal();
    } else {
        window_.show();
    }
    window_.raise();
    window_.requestActivate();
}

void TrayController::updateTrayState() {
    const bool meshActive = viewModel_.meshVisible();
    if (meshActive && QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon_->show();
    } else {
        if (!window_.isVisible() && !quitting_) {
            restoreWindow();
        }
        trayIcon_->hide();
    }

    const bool callActive = viewModel_.callActive();
    muteAction_->setVisible(callActive);
    muteAction_->setEnabled(callActive);
    muteAction_->setText(viewModel_.muted() ? tr("Включить микрофон")
                                            : tr("Выключить микрофон"));
    leaveMeshAction_->setEnabled(meshActive);
}

} // namespace tmc
