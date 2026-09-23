#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace tmc::signaling_protocol {

inline constexpr int ProtocolVersion = 1;

struct Envelope {
    int version{ProtocolVersion};
    QString type;
    std::optional<QString> requestId;
    QJsonObject body;
};

} // namespace tmc::signaling_protocol
