#pragma once

#include "tmc/signaling_protocol/envelope.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <variant>

namespace tmc::server {

struct Delivery {
    QString sessionId;
    signaling_protocol::Envelope message;
};

// RAM-only application state. No sockets, SDP parsing or client dependencies.
// All calls run on the server event-loop thread. Time is monotonic milliseconds.
class RoomRegistry {
public:
    struct CreatedInvitation {
        QString token;
        int expiresInSeconds;
    };
    enum class InvitationError { NotInRoom, RoomMismatch, ResourceLimit, TokenGenerationFailed };
    using InvitationResult = std::variant<CreatedInvitation, InvitationError>;

    [[nodiscard]] InvitationResult createInvitation(const QString& sessionId, const QString& roomId,
                                                    qint64 now);
    [[nodiscard]] QVector<Delivery> handle(const QString& sessionId,
                                           const signaling_protocol::Envelope& request,
                                           qint64 now);
    [[nodiscard]] QVector<Delivery> disconnect(const QString& sessionId);
    void expireInvitations(qint64 now);
    void clear();
    [[nodiscard]] QString roomForSession(const QString& sessionId) const;
    [[nodiscard]] bool canJoinRoom(const QString& roomId) const;

private:
    struct Membership {
        QString roomId;
        QString peerId;
    };
    struct Invitation {
        QString issuerPeerId;
        qint64 expiresAt;
    };
    struct Room {
        QHash<QString, QString> peers; // membership peerId -> WebSocket sessionId
        QHash<QString, Invitation> invitations; // bearer token -> invitation
    };

    [[nodiscard]] QVector<Delivery> leave(const QString& sessionId, const QString& reason);
    QHash<QString, Membership> memberships_;
    QHash<QString, Room> rooms_;
};

} // namespace tmc::server
