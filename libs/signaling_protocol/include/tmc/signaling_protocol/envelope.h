#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace tmc::signaling_protocol {

struct Envelope {
    int version{1};
    QString type;
    std::optional<QString> requestId;
    QJsonObject body;
};

} // namespace tmc::signaling_protocol
