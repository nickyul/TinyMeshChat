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
    PeerProof,
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
    SignalingState,
    SignalingJoinRequest,
    SignalingJoinInvitation,
    Ping,
    Pong
};

struct HelloPayload {
    QString displayName;
    int signalingVersion{0};
    QString publicKey;
    QString nonce;
};

struct PeerProofPayload { QString signature; };

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

// Direct control-channel messages. No routing and no endpoint discovery:
// recovery only uses the signaling server already authorized on this client.
struct SignalingStatePayload {
    QString server;
    QString sessionId;
    QString roomId;
};

struct SignalingJoinPayload {
    QString requestId;
    QString sessionId;
    QString roomId;
    QString token; // Empty on a request; one-use bearer token on a reply.
};

using PacketPayload =
    std::variant<HelloPayload, PeerProofPayload, PeerSnapshotPayload, PeerAnnouncePayload, PeerLeavePayload,
                 ChatMessagePayload, ChatAckPayload, VoiceStatePayload, VoiceQualityPayload,
                 RoutePayload, LinkSignalingPayload, SessionSignalingPayload,
                 HeartbeatPayload, SignalingStatePayload, SignalingJoinPayload>;

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
