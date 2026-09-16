#include "tmc/protocol/packet.h"

#include <array>

namespace tmc {

namespace {

struct PacketTypeName {
    PacketType type;
    const char* name;
};

constexpr std::array PacketTypeNames{
    PacketTypeName{PacketType::PeerHello, "peer.hello"},
    PacketTypeName{PacketType::PeerSnapshot, "peer.snapshot"},
    PacketTypeName{PacketType::PeerAnnounce, "peer.announce"},
    PacketTypeName{PacketType::PeerLeave, "peer.leave"},
    PacketTypeName{PacketType::ChatMessage, "chat.message"},
    PacketTypeName{PacketType::ChatAck, "chat.ack"},
    PacketTypeName{PacketType::VoiceState, "voice.state"},
    PacketTypeName{PacketType::VoiceQuality, "voice.quality"},
    PacketTypeName{PacketType::RouteRequest, "route.request"},
    PacketTypeName{PacketType::RouteReply, "route.reply"},
    PacketTypeName{PacketType::LinkOffer, "link.offer"},
    PacketTypeName{PacketType::LinkAnswer, "link.answer"},
    PacketTypeName{PacketType::SessionOffer, "session.offer"},
    PacketTypeName{PacketType::SessionAnswer, "session.answer"},
    PacketTypeName{PacketType::RendezvousMetadata, "rendezvous.metadata"},
    PacketTypeName{PacketType::Ping, "ping"},
    PacketTypeName{PacketType::Pong, "pong"},
};

} // namespace

QString toString(PacketType type) {
    for (const auto& entry : PacketTypeNames) {
        if (entry.type == type) {
            return QString::fromLatin1(entry.name);
        }
    }
    return {};
}

std::optional<PacketType> packetTypeFromString(const QString& type) {
    for (const auto& entry : PacketTypeNames) {
        if (type == QLatin1StringView(entry.name)) {
            return entry.type;
        }
    }
    return std::nullopt;
}

} // namespace tmc
