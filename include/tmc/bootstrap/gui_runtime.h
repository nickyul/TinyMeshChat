#pragma once

#include "tmc/bootstrap/application_options.h"

#include <memory>

class QApplication;
class QQmlApplicationEngine;

namespace tmc {

class ApplicationController;
class AppLinkController;
class AppViewModel;
class TrayController;
class UpdateService;

class GuiRuntime final {
public:
    GuiRuntime(int& argc, char** argv, ApplicationOptions options);
    ~GuiRuntime();

    int run();

private:
    void bringMainWindowToFront();
    void wireAppLinks();

    int& argc_;
    char** argv_;
    ApplicationOptions options_;
    std::unique_ptr<QApplication> application_;
    std::unique_ptr<AppLinkController> appLinks_;
    std::unique_ptr<ApplicationController> controller_;
    std::unique_ptr<UpdateService> updates_;
    std::unique_ptr<AppViewModel> viewModel_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    std::unique_ptr<TrayController> tray_;
};

} // namespace tmc
