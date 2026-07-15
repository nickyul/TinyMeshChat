#pragma once
#include "identity/peer_identity.h"
#include <QDateTime>
#include <QString>
namespace tmc {
struct Invitation {
    enum class Kind { Offer, Answer };
    Kind kind{Kind::Offer};
    QString roomId, roomName, connectionId, sdp, nonce;
    PeerIdentity fromPeer;
    QDateTime createdAt, expiresAt;
    bool isExpired(const QDateTime& now = QDateTime::currentDateTimeUtc()) const {
        return expiresAt < now;
    }
};
} // namespace tmc
