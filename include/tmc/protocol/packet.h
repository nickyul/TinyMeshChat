#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace tmc {

struct Packet {
    QString type, packetId, meshId, senderId;
    QDateTime createdAt;
    QJsonObject payload;
};

} // namespace tmc
