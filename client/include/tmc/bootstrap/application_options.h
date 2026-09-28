#pragma once

#include <QString>

namespace tmc {

struct ApplicationOptions {
    bool console{false};
    bool qmlSmoke{false};
    QString displayName;
    QString error;
};

ApplicationOptions parseApplicationOptions(int argc, char** argv);

} // namespace tmc
