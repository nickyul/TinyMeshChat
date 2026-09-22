#pragma once

#include "room_registry.h"
#include <QJsonArray>
#include <QSet>

namespace tmc::server {

// SignalingServer binds every published identity to its authenticated WebSocket session.
// Only live sessions own presence, acquaintance lists and invitation state.
class PresenceRegistry {
public:
    QVector<Delivery> handle(const QString& sessionId, const signaling_protocol::Envelope& request,
                             RoomRegistry& rooms, qint64 now);
    QVector<Delivery> maintain(const RoomRegistry& rooms, qint64 now);
    QVector<Delivery> disconnect(const QString& sessionId, const RoomRegistry& rooms, qint64 now);
    void clear();

private:
    struct Profile {
        QString identityId;
        QString displayName;
        QSet<QString> known;
        bool busy{false};
        QJsonArray snapshot;
        bool published{false};
        QHash<QString, qint64> lastInvite;
    };
    struct Invitation {
        QString id, fromSession, toSession, toIdentity, roomId, meshId;
        qint64 expiresAt{0};
    };
    bool mutual(const QString& first, const QString& second) const;
    QVector<Delivery> finish(const Invitation& invitation, const QString& status) const;
    QHash<QString, Profile> profiles_;
    QHash<QString, QString> sessionsByIdentity_;
    QHash<QString, Invitation> invitations_;
};

} // namespace tmc::server
