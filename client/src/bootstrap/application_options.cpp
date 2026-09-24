#include "tmc/bootstrap/application_options.h"

namespace tmc {

ApplicationOptions parseApplicationOptions(int argc, char** argv) {
    ApplicationOptions options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = QString::fromLocal8Bit(argv[index]);
        if (argument == "--console") {
            options.console = true;
        } else if (argument == "--qml-smoke") {
            options.qmlSmoke = true;
        } else if (argument == "--display-name") {
            if (index + 1 >= argc) {
                options.error = "--display-name requires a value";
                break;
            }
            options.displayName = QString::fromLocal8Bit(argv[++index]);
        } else if (argument.startsWith("tinymesh://")) {
            options.appLink = QUrl(argument);
        }
    }
    return options;
}

} // namespace tmc
