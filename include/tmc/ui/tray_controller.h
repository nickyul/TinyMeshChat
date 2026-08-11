#pragma once

#include <QObject>

#include <memory>

class QAction;
class QEvent;
class QMenu;
class QSystemTrayIcon;
class QWindow;

namespace tmc {

class AppViewModel;

class TrayController final : public QObject {
    Q_OBJECT

public:
    TrayController(QWindow& window, AppViewModel& viewModel, QObject* parent = nullptr);
    ~TrayController() override;

    void quitApplication();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void restoreWindow();
    void updateTrayState();

    QWindow& window_;
    AppViewModel& viewModel_;
    std::unique_ptr<QMenu> menu_;
    std::unique_ptr<QSystemTrayIcon> trayIcon_;
    QAction* muteAction_{nullptr};
    QAction* leaveMeshAction_{nullptr};
    bool quitting_{false};
};

} // namespace tmc
