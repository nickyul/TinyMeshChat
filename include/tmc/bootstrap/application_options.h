#pragma once

#include <QString>
#include <QUrl>

namespace tmc {

struct ApplicationOptions {
    bool console{false};
    bool qmlSmoke{false};
    QString displayName;
    QUrl appLink;
    QString error;
};

ApplicationOptions parseApplicationOptions(int argc, char** argv);

} // namespace tmc
