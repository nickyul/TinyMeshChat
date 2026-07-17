#pragma once

#include "tmc/identity/peer_identity.h"

#include <QDateTime>
#include <QString>

namespace tmc {

struct Invitation {
    enum class Kind { Offer, Answer };
    Kind kind{Kind::Offer};
    QString meshId, connectionId, sdp, nonce;
    PeerIdentity fromPeer;
    QDateTime createdAt, expiresAt;

    bool isExpired(const QDateTime& now = QDateTime::currentDateTimeUtc()) const;
};

} // namespace tmc
