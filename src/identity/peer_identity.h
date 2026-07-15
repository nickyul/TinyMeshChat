#pragma once
#include <QDateTime>
#include <QString>
namespace tmc {
struct PeerIdentity {
    QString peerId, displayName, deviceId;
    QDateTime createdAt;
    bool isValid() const;
};
} // namespace tmc
