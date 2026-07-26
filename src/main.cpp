#include "tmc/bootstrap/application_options.h"
#include "tmc/bootstrap/console_runtime.h"
#include "tmc/bootstrap/gui_runtime.h"

#include <utility>

int main(int argc, char** argv) {
    auto options = tmc::parseApplicationOptions(argc, argv);
    if (options.console) {
        return tmc::ConsoleRuntime(argc, argv, std::move(options)).run();
    }
    return tmc::GuiRuntime(argc, argv, std::move(options)).run();
}
