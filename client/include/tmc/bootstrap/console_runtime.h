#pragma once

#include "tmc/bootstrap/application_options.h"

namespace tmc {

class ConsoleRuntime final {
public:
    ConsoleRuntime(int& argc, char** argv, ApplicationOptions options);

    int run();

private:
    static void prepareNativeConsole();

    int& argc_;
    char** argv_;
    ApplicationOptions options_;
};

} // namespace tmc
