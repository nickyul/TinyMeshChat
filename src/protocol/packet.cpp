#include "tmc/protocol/packet.h"

#include <type_traits>

namespace tmc {

QString toString(PacketType type) {
    switch (type) {
    case PacketType::PeerHello:
        return "peer.hello";
    case PacketType::PeerSnapshot:
        return "peer.snapshot";
    case PacketType::PeerAnnounce:
        return "peer.announce";
    case PacketType::PeerLeave:
        return "peer.leave";
    case PacketType::ChatMessage:
        return "chat.message";
    case PacketType::ChatAck:
        return "chat.ack";
    case PacketType::VoiceState:
        return "voice.state";
    case PacketType::VoiceQuality:
        return "voice.quality";
    case PacketType::RouteRequest:
        return "route.request";
    case PacketType::RouteReply:
        return "route.reply";
    case PacketType::LinkOffer:
        return "link.offer";
    case PacketType::LinkAnswer:
        return "link.answer";
    case PacketType::SessionOffer:
        return "session.offer";
    case PacketType::SessionAnswer:
        return "session.answer";
    case PacketType::Ping:
        return "ping";
    case PacketType::Pong:
        return "pong";
    }
    return {};
}

std::optional<PacketType> packetTypeFromString(const QString& type) {
    for (const auto candidate :
         {PacketType::PeerHello, PacketType::PeerSnapshot, PacketType::PeerAnnounce,
          PacketType::PeerLeave, PacketType::ChatMessage,
          PacketType::ChatAck, PacketType::VoiceState, PacketType::VoiceQuality,
          PacketType::RouteRequest,
          PacketType::RouteReply, PacketType::LinkOffer, PacketType::LinkAnswer,
          PacketType::SessionOffer, PacketType::SessionAnswer, PacketType::Ping,
          PacketType::Pong}) {
        if (toString(candidate) == type) {
            return candidate;
        }
    }
    return std::nullopt;
}

QJsonObject payloadToJson(const PacketPayload& payload) {
    return std::visit(
        [](const auto& value) -> QJsonObject {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, EmptyPayload>) {
                return {};
            } else if constexpr (std::is_same_v<T, HelloPayload>) {
                return {{"display_name", value.displayName},
                        {"device_id", value.deviceId},
                        {"identity_created_at",
                         value.identityCreatedAt.toUTC().toString(Qt::ISODateWithMs)}};
            } else if constexpr (std::is_same_v<T, PeerSnapshotPayload>) {
                return {{"revision", value.revision}, {"peers", value.peers}};
            } else if constexpr (std::is_same_v<T, PeerAnnouncePayload>) {
                const QJsonObject peer{
                    {"id", value.peer.peerId},
                    {"name", value.peer.displayName},
                    {"device", value.peer.deviceId},
                    {"created_at", value.peer.createdAt.toUTC().toString(Qt::ISODateWithMs)}};
                return {{"peer", peer}, {"epoch", value.epoch}, {"hops", value.hops}};
            } else if constexpr (std::is_same_v<T, PeerLeavePayload>) {
                return {{"reason", value.reason}};
            } else if constexpr (std::is_same_v<T, ChatMessagePayload>) {
                return {{"message_id", value.messageId},
                        {"text", value.text},
                        {"logical_clock", value.logicalClock}};
            } else if constexpr (std::is_same_v<T, ChatAckPayload>) {
                return {{"message_id", value.messageId}};
            } else if constexpr (std::is_same_v<T, VoiceStatePayload>) {
                return {{"joined", value.joined}, {"muted", value.muted}};
            } else if constexpr (std::is_same_v<T, VoiceQualityPayload>) {
                return {{"loss_percent", value.packetLossPercent},
                        {"jitter_ms", value.jitterMs},
                        {"buffer_ms", value.bufferMs}};
            } else if constexpr (std::is_same_v<T, RoutePayload>) {
                return {{"request", value.requestId}, {"hops", value.hops}};
            } else if constexpr (std::is_same_v<T, LinkSignalingPayload>) {
                return {{"link", value.connectionId},
                        {"generation", static_cast<qint64>(value.generation)},
                        {"sdp", value.sdp}};
            } else if constexpr (std::is_same_v<T, SessionSignalingPayload>) {
                QJsonObject result{{"link", value.connectionId},
                                   {"negotiation", static_cast<qint64>(value.negotiation)},
                                   {"sdp", value.sdp}};
                if (!value.reason.isEmpty()) {
                    result.insert("reason", value.reason);
                }
                return result;
            } else {
                return {{"nonce", value.nonce},
                        {"sent_at", value.sentAt.toUTC().toString(Qt::ISODateWithMs)}};
            }
        },
        payload);
}

} // namespace tmc
