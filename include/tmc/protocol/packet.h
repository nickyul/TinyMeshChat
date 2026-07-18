#pragma once

#include "tmc/identity/peer_identity.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>
#include <variant>

namespace tmc {

enum class PacketType {
    PeerHello,
    PeerHelloAck,
    PeerList,
    ChatMessage,
    ChatAck,
    VoiceState,
    MeshOffer,
    MeshAnswer,
    Ping,
    Pong
};

struct EmptyPayload {};

struct HelloPayload {
    QString displayName;
    QString deviceId;
};

struct PeerListPayload {
    QJsonArray peers;
};

struct ChatMessagePayload {
    QString messageId;
    QString text;
    qint64 logicalClock{0};
};

struct ChatAckPayload {
    QString messageId;
};

struct VoiceStatePayload {
    bool joined{false};
    bool muted{false};
};

struct MeshSignalingPayload {
    QString phase;
    QString routeId;
    int hopCount{0};
    QString connectionId;
    PeerIdentity fromPeer;
    QString targetPeerId;
    QString sdp;
};

struct HeartbeatPayload {
    QString nonce;
    QDateTime sentAt;
};

using PacketPayload =
    std::variant<EmptyPayload, HelloPayload, PeerListPayload, ChatMessagePayload, ChatAckPayload,
                 VoiceStatePayload, MeshSignalingPayload, HeartbeatPayload>;

struct Packet {
    PacketType type{PacketType::PeerHelloAck};
    QString packetId;
    QString meshId;
    QString senderId;
    QDateTime createdAt;
    PacketPayload payload;
};

QString toString(PacketType type);
std::optional<PacketType> packetTypeFromString(const QString& type);
QJsonObject payloadToJson(const PacketPayload& payload);

} // namespace tmc
