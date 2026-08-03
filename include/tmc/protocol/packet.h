#pragma once

#include "tmc/identity/peer_identity.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>
#include <variant>

namespace tmc {

enum class PacketType {
    PeerHello,
    PeerSnapshot,
    PeerAnnounce,
    PeerLeave,
    ChatMessage,
    ChatAck,
    VoiceState,
    VoiceQuality,
    RouteRequest,
    RouteReply,
    LinkOffer,
    LinkAnswer,
    SessionOffer,
    SessionAnswer,
    Ping,
    Pong
};

struct HelloPayload {
    QString displayName;
};

struct PeerSnapshotPayload {
    QList<PeerIdentity> peers;
};

struct PeerAnnouncePayload {
    PeerIdentity peer;
    int hops{0};
};

struct PeerLeavePayload {
    QString reason;
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

struct VoiceQualityPayload {
    double packetLossPercent{0.0};
    int jitterMs{0};
    int bufferMs{0};
};

struct RoutePayload {
    QString requestId;
    int hops{0};
};

struct LinkSignalingPayload {
    QString connectionId;
    quint64 generation{0};
    QString sdp;
};

struct SessionSignalingPayload {
    QString connectionId;
    quint64 negotiation{0};
    QString sdp;
};

struct HeartbeatPayload {
    QString nonce;
    QDateTime sentAt;
};

using PacketPayload =
    std::variant<HelloPayload, PeerSnapshotPayload, PeerAnnouncePayload, PeerLeavePayload,
                 ChatMessagePayload, ChatAckPayload, VoiceStatePayload, VoiceQualityPayload,
                 RoutePayload, LinkSignalingPayload, SessionSignalingPayload,
                 HeartbeatPayload>;

struct Packet {
    PacketType type{PacketType::PeerHello};
    QString packetId;
    QString meshId;
    QString senderId;
    QDateTime createdAt;
    PacketPayload payload;
    QString targetId;
    int ttl{0};
};

QString toString(PacketType type);
std::optional<PacketType> packetTypeFromString(const QString& type);

} // namespace tmc
