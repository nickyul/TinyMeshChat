#include "tmc/protocol/packet.h"

#include <type_traits>

namespace tmc {

QString toString(PacketType type) {
    switch (type) {
    case PacketType::PeerHello:
        return "peer.hello";
    case PacketType::PeerHelloAck:
        return "peer.hello_ack";
    case PacketType::PeerList:
        return "peer.list";
    case PacketType::ChatMessage:
        return "chat.message";
    case PacketType::ChatAck:
        return "chat.ack";
    case PacketType::VoiceState:
        return "voice.state";
    case PacketType::MeshOffer:
        return "mesh.offer";
    case PacketType::MeshAnswer:
        return "mesh.answer";
    case PacketType::Ping:
        return "ping";
    case PacketType::Pong:
        return "pong";
    }
    return {};
}

std::optional<PacketType> packetTypeFromString(const QString& type) {
    for (const auto candidate :
         {PacketType::PeerHello, PacketType::PeerHelloAck, PacketType::PeerList,
          PacketType::ChatMessage, PacketType::ChatAck, PacketType::VoiceState,
          PacketType::MeshOffer, PacketType::MeshAnswer, PacketType::Ping, PacketType::Pong}) {
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
                return {{"display_name", value.displayName}, {"device_id", value.deviceId}};
            } else if constexpr (std::is_same_v<T, PeerListPayload>) {
                return {{"peers", value.peers}};
            } else if constexpr (std::is_same_v<T, ChatMessagePayload>) {
                return {{"message_id", value.messageId},
                        {"text", value.text},
                        {"logical_clock", value.logicalClock}};
            } else if constexpr (std::is_same_v<T, ChatAckPayload>) {
                return {{"message_id", value.messageId}};
            } else if constexpr (std::is_same_v<T, VoiceStatePayload>) {
                return {{"joined", value.joined}, {"muted", value.muted}};
            } else if constexpr (std::is_same_v<T, MeshSignalingPayload>) {
                const QJsonObject peer{
                    {"peer_id", value.fromPeer.peerId},
                    {"display_name", value.fromPeer.displayName},
                    {"device_id", value.fromPeer.deviceId},
                    {"created_at", value.fromPeer.createdAt.toUTC().toString(Qt::ISODateWithMs)}};
                return {{"phase", value.phase},
                        {"route_id", value.routeId},
                        {"hop_count", value.hopCount},
                        {"connection_id", value.connectionId},
                        {"from_peer", peer},
                        {"target_peer_id", value.targetPeerId},
                        {"sdp", value.sdp}};
            } else {
                return {{"nonce", value.nonce},
                        {"sent_at", value.sentAt.toUTC().toString(Qt::ISODateWithMs)}};
            }
        },
        payload);
}

} // namespace tmc
