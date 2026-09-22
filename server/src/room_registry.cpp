#include "room_registry.h"

#include "tmc/signaling_protocol/message_codec.h"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QUuid>

#include <algorithm>

namespace tmc::server {

QString RoomRegistry::roomForSession(const QString& sessionId) const {
    return memberships_.value(sessionId).roomId;
}

bool RoomRegistry::canJoinRoom(const QString& roomId) const {
    const auto room = rooms_.constFind(roomId);
    return room != rooms_.cend() && room->peers.size() < signaling_protocol::RoomCapacity;
}

namespace {

using signaling_protocol::Envelope;
using signaling_protocol::MessageCodec;

constexpr qsizetype MaxRooms = 16;
constexpr qsizetype MaxInvitationsPerRoom = 16;

QString newUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString newToken() {
    QByteArray bytes;
    bytes.reserve(32);
    for (int word = 0; word < 8; ++word) {
        const auto random = QRandomGenerator::system()->generate();
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.append(static_cast<char>((random >> shift) & 0xff));
        }
    }
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                             QByteArray::OmitTrailingEquals));
}

Envelope event(const QString& type, const QJsonObject& body) {
    return {1, type, std::nullopt, body};
}

} // namespace

QVector<Delivery> RoomRegistry::handle(const QString& sessionId, const Envelope& request,
                                      qint64 now) {
    expireInvitations(now);
    const auto fail = [&](const QString& code) -> QVector<Delivery> {
        return {{sessionId, MessageCodec::error(request.requestId, code)}};
    };
    const auto reply = [&](const QString& type, const QJsonObject& body) -> Delivery {
        return {sessionId, {1, type, request.requestId, body}};
    };

    // The registry also guards its boundary for callers other than the transport.
    if (MessageCodec::validateRequest(request)) {
        return fail(QStringLiteral("invalid_message"));
    }
    if (request.type == "room.create") {
        if (memberships_.contains(sessionId)) {
            return fail(QStringLiteral("busy"));
        }
        if (rooms_.size() >= MaxRooms) {
            return fail(QStringLiteral("resource_limit"));
        }
        QString roomId;
        do {
            roomId = newUuid();
        } while (rooms_.contains(roomId));
        const auto peerId = newUuid();
        Room room;
        room.peers.insert(peerId, sessionId);
        rooms_.insert(roomId, room);
        memberships_.insert(sessionId, {roomId, peerId});
        return {reply(QStringLiteral("room.created"), {{"roomId", roomId}, {"peerId", peerId}})};
    }

    const auto roomId = request.body.value("roomId").toString();
    if (request.type == "room.join") {
        if (memberships_.contains(sessionId)) {
            return fail(QStringLiteral("busy"));
        }
        auto room = rooms_.find(roomId);
        const auto token = request.body.value("token").toString();
        if (room == rooms_.end() || !room->invitations.contains(token)) {
            return fail(QStringLiteral("invalid_invitation"));
        }
        if (room->peers.size() >= signaling_protocol::RoomCapacity) {
            return fail(QStringLiteral("room_full"));
        }
        QString peerId;
        do {
            peerId = newUuid();
        } while (room->peers.contains(peerId));
        auto peerIds = room->peers.keys();
        std::sort(peerIds.begin(), peerIds.end());
        QJsonArray peers;
        for (const auto& id : peerIds) {
            peers.append(id);
        }
        QVector<Delivery> deliveries{
            reply(QStringLiteral("room.joined"),
                  {{"roomId", roomId}, {"peerId", peerId}, {"peers", peers}})};
        for (const auto& existingSession : room->peers) {
            deliveries.append({existingSession, event(QStringLiteral("peer.joined"),
                                                       {{"roomId", roomId}, {"peerId", peerId}})});
        }
        // Token consumption and membership insertion happen together on one thread.
        room->invitations.remove(token);
        room->peers.insert(peerId, sessionId);
        memberships_.insert(sessionId, {roomId, peerId});
        return deliveries;
    }

    const auto member = memberships_.constFind(sessionId);
    if (member == memberships_.cend()) {
        return fail(QStringLiteral("not_in_room"));
    }
    if (member->roomId != roomId) {
        return fail(QStringLiteral("room_mismatch"));
    }
    auto& room = rooms_[roomId];
    if (request.type == "invite.create") {
        if (room.invitations.size() >= MaxInvitationsPerRoom) {
            return fail(QStringLiteral("resource_limit"));
        }
        QString token;
        do {
            token = newToken();
        } while (room.invitations.contains(token));
        room.invitations.insert(token, {member->peerId,
            now + signaling_protocol::InvitationLifetimeSeconds * qint64{1000}});
        return {reply(QStringLiteral("invite.created"),
                      {{"roomId", roomId}, {"token", token},
                       {"expiresInSeconds", signaling_protocol::InvitationLifetimeSeconds}})};
    }
    if (request.type == "room.leave") {
        auto deliveries = leave(sessionId, QStringLiteral("leave"));
        deliveries.prepend(reply(QStringLiteral("room.left"), {{"roomId", roomId}}));
        return deliveries;
    }
    if (request.type == "signal.send") {
        const auto recipient = request.body.value("toPeerId").toString();
        if (recipient == member->peerId) {
            return fail(QStringLiteral("invalid_target"));
        }
        if (!room.peers.contains(recipient)) {
            return fail(QStringLiteral("peer_not_found"));
        }
        return {{room.peers.value(recipient), event(QStringLiteral("signal.received"),
                    {{"roomId", roomId}, {"fromPeerId", member->peerId},
                     {"payload", request.body.value("payload")}})},
                reply(QStringLiteral("signal.accepted"), {})};
    }
    return fail(QStringLiteral("unknown_type"));
}

QVector<Delivery> RoomRegistry::leave(const QString& sessionId, const QString& reason) {
    const auto member = memberships_.find(sessionId);
    if (member == memberships_.end()) {
        return {};
    }
    const auto roomId = member->roomId;
    const auto peerId = member->peerId;
    memberships_.erase(member);
    auto room = rooms_.find(roomId);
    if (room == rooms_.end()) {
        return {};
    }
    room->peers.remove(peerId);
    for (auto invite = room->invitations.begin(); invite != room->invitations.end();) {
        if (invite->issuerPeerId == peerId) {
            invite = room->invitations.erase(invite);
        } else {
            ++invite;
        }
    }
    QVector<Delivery> deliveries;
    for (const auto& remainingSession : room->peers) {
        deliveries.append({remainingSession, event(QStringLiteral("peer.left"),
            {{"roomId", roomId}, {"peerId", peerId}, {"reason", reason}})});
    }
    if (room->peers.isEmpty()) {
        rooms_.erase(room);
    }
    return deliveries;
}

QVector<Delivery> RoomRegistry::disconnect(const QString& sessionId) {
    return leave(sessionId, QStringLiteral("disconnect"));
}

void RoomRegistry::expireInvitations(qint64 now) {
    for (auto& room : rooms_) {
        for (auto invite = room.invitations.begin(); invite != room.invitations.end();) {
            if (invite->expiresAt <= now) {
                invite = room.invitations.erase(invite);
            } else {
                ++invite;
            }
        }
    }
}

void RoomRegistry::clear() {
    memberships_.clear();
    rooms_.clear();
}

} // namespace tmc::server
