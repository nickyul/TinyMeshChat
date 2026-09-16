#include "tmc/protocol/packet_codec.h"

#include "tmc/core/limits.h"
#include "tmc/core/uuid.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <type_traits>

namespace tmc {

namespace {

bool validUuid(const QJsonValue& value) {
    return value.isString() && isCanonicalUuid(value.toString());
}

bool validTimestamp(const QJsonValue& value) {
    return value.isString() && QDateTime::fromString(value.toString(), Qt::ISODateWithMs).isValid();
}

QJsonObject identityToJson(const PeerIdentity& identity) {
    return {{"id", identity.peerId}, {"name", identity.displayName}};
}

Result<PeerIdentity> decodeIdentity(const QJsonValue& value) {
    if (!value.isObject()) {
        return Result<PeerIdentity>::failure("Invalid peer identity");
    }
    const auto object = value.toObject();
    PeerIdentity identity{object.value("id").toString(), object.value("name").toString()};
    if (!identity.isValid() ||
        identity.displayName.size() > limits::MaxDisplayNameLength) {
        return Result<PeerIdentity>::failure("Invalid peer identity");
    }
    return Result<PeerIdentity>::success(identity);
}

bool payloadMatchesType(PacketType type, const PacketPayload& payload) {
    switch (type) {
    case PacketType::PeerHello:
        return std::holds_alternative<HelloPayload>(payload);
    case PacketType::PeerSnapshot:
        return std::holds_alternative<PeerSnapshotPayload>(payload);
    case PacketType::PeerAnnounce:
        return std::holds_alternative<PeerAnnouncePayload>(payload);
    case PacketType::PeerLeave:
        return std::holds_alternative<PeerLeavePayload>(payload);
    case PacketType::ChatMessage:
        return std::holds_alternative<ChatMessagePayload>(payload);
    case PacketType::ChatAck:
        return std::holds_alternative<ChatAckPayload>(payload);
    case PacketType::VoiceState:
        return std::holds_alternative<VoiceStatePayload>(payload);
    case PacketType::VoiceQuality:
        return std::holds_alternative<VoiceQualityPayload>(payload);
    case PacketType::RouteRequest:
    case PacketType::RouteReply:
        return std::holds_alternative<RoutePayload>(payload);
    case PacketType::LinkOffer:
    case PacketType::LinkAnswer:
        return std::holds_alternative<LinkSignalingPayload>(payload);
    case PacketType::SessionOffer:
    case PacketType::SessionAnswer:
        return std::holds_alternative<SessionSignalingPayload>(payload);
    case PacketType::RendezvousMetadata:
        return std::holds_alternative<RendezvousMetadataPayload>(payload);
    case PacketType::Ping:
    case PacketType::Pong:
        return std::holds_alternative<HeartbeatPayload>(payload);
    }
    return false;
}

QJsonObject payloadToJson(const PacketPayload& payload) {
    return std::visit(
        [](const auto& value) -> QJsonObject {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, HelloPayload>) {
                return {{"display_name", value.displayName}};
            } else if constexpr (std::is_same_v<T, PeerSnapshotPayload>) {
                QJsonArray peers;
                for (const auto& peer : value.peers) {
                    peers.append(identityToJson(peer));
                }
                return {{"peers", peers}};
            } else if constexpr (std::is_same_v<T, PeerAnnouncePayload>) {
                return {{"peer", identityToJson(value.peer)}, {"hops", value.hops}};
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
                return {{"link", value.connectionId},
                        {"negotiation", static_cast<qint64>(value.negotiation)},
                        {"sdp", value.sdp}};
            } else if constexpr (std::is_same_v<T, RendezvousMetadataPayload>) {
                return {{"secret", value.secret},
                        {"public_address", value.publicAddress},
                        {"public_port", value.publicPort},
                        {"local_address", value.localAddress},
                        {"local_port", value.localPort},
                        {"mapping_method", value.mappingMethod},
                        {"acknowledgement", value.acknowledgement}};
            } else {
                static_assert(std::is_same_v<T, HeartbeatPayload>);
                return {{"nonce", value.nonce},
                        {"sent_at", value.sentAt.toUTC().toString(Qt::ISODateWithMs)}};
            }
        },
        payload);
}

Result<PacketPayload> decodePayload(PacketType type, const QJsonObject& payload) {
    switch (type) {
    case PacketType::PeerHello: {
        const auto displayName = payload.value("display_name").toString();
        if (displayName.trimmed().isEmpty() ||
            displayName.size() > limits::MaxDisplayNameLength) {
            break;
        }
        return Result<PacketPayload>::success(HelloPayload{displayName});
    }
    case PacketType::PeerSnapshot: {
        if (!payload.value("peers").isArray()) {
            break;
        }
        const auto peersJson = payload.value("peers").toArray();
        if (peersJson.isEmpty() || peersJson.size() > 16) {
            break;
        }
        QList<PeerIdentity> peers;
        peers.reserve(peersJson.size());
        QSet<QString> unique;
        for (const auto& peer : peersJson) {
            const auto decoded = decodeIdentity(peer);
            if (!decoded || unique.contains(decoded.value().peerId)) {
                return Result<PacketPayload>::failure("Invalid peer.snapshot payload");
            }
            unique.insert(decoded.value().peerId);
            peers.append(decoded.value());
        }
        return Result<PacketPayload>::success(PeerSnapshotPayload{peers});
    }
    case PacketType::PeerAnnounce: {
        const auto peer = decodeIdentity(payload.value("peer"));
        const auto hops = payload.value("hops").toInt(-1);
        if (peer && hops >= 0 && hops <= 16) {
            return Result<PacketPayload>::success(PeerAnnouncePayload{peer.value(), hops});
        }
        break;
    }
    case PacketType::PeerLeave: {
        const auto reason = payload.value("reason").toString();
        if (reason.size() <= 128) {
            return Result<PacketPayload>::success(PeerLeavePayload{reason});
        }
        break;
    }
    case PacketType::ChatMessage: {
        const auto messageId = payload.value("message_id").toString();
        const auto text = payload.value("text").toString();
        const auto clock = payload.value("logical_clock").toInteger();
        if (!isCanonicalUuid(messageId) || text.trimmed().isEmpty() ||
            text.size() > limits::MaxChatMessageLength || clock <= 0) {
            break;
        }
        return Result<PacketPayload>::success(ChatMessagePayload{messageId, text, clock});
    }
    case PacketType::ChatAck: {
        const auto messageId = payload.value("message_id").toString();
        if (isCanonicalUuid(messageId)) {
            return Result<PacketPayload>::success(ChatAckPayload{messageId});
        }
        break;
    }
    case PacketType::VoiceState:
        if (payload.value("joined").isBool() && payload.value("muted").isBool()) {
            return Result<PacketPayload>::success(VoiceStatePayload{
                payload.value("joined").toBool(), payload.value("muted").toBool()});
        }
        break;
    case PacketType::VoiceQuality: {
        const auto loss = payload.value("loss_percent").toDouble(-1.0);
        const auto jitter = payload.value("jitter_ms").toInt(-1);
        const auto buffer = payload.value("buffer_ms").toInt(-1);
        if (loss >= 0.0 && loss <= 100.0 && jitter >= 0 && jitter <= 5000 &&
            buffer >= 0 && buffer <= 1000) {
            return Result<PacketPayload>::success(VoiceQualityPayload{loss, jitter, buffer});
        }
        break;
    }
    case PacketType::RouteRequest:
    case PacketType::RouteReply: {
        const auto requestId = payload.value("request").toString();
        const auto hops = payload.value("hops").toInt(-1);
        if (isCanonicalUuid(requestId) && hops >= 0 && hops <= 16) {
            return Result<PacketPayload>::success(RoutePayload{requestId, hops});
        }
        break;
    }
    case PacketType::LinkOffer:
    case PacketType::LinkAnswer: {
        const auto connectionId = payload.value("link").toString();
        const auto generation = payload.value("generation").toInteger(-1);
        const auto sdp = payload.value("sdp").toString();
        if (!isCanonicalUuid(connectionId) || generation < 0 || sdp.isEmpty() ||
            sdp.toUtf8().size() >= PacketCodec::MaxBytes) {
            break;
        }
        return Result<PacketPayload>::success(
            LinkSignalingPayload{connectionId, static_cast<quint64>(generation), sdp});
    }
    case PacketType::SessionOffer:
    case PacketType::SessionAnswer: {
        const auto connectionId = payload.value("link").toString();
        const auto negotiation = payload.value("negotiation").toInteger(-1);
        const auto sdp = payload.value("sdp").toString();
        if (isCanonicalUuid(connectionId) && negotiation >= 0 && !sdp.isEmpty() &&
            sdp.toUtf8().size() < PacketCodec::MaxBytes) {
            return Result<PacketPayload>::success(SessionSignalingPayload{
                connectionId, static_cast<quint64>(negotiation), sdp});
        }
        break;
    }
    case PacketType::RendezvousMetadata: {
        const auto secret = payload.value("secret").toString();
        const auto publicAddress = payload.value("public_address").toString();
        const auto publicPort = payload.value("public_port").toInt(-1);
        const auto localAddress = payload.value("local_address").toString();
        const auto localPort = payload.value("local_port").toInt(-1);
        const auto mappingMethod = payload.value("mapping_method").toString();
        if (secret.size() <= 64 && publicAddress.size() <= 64 && localAddress.size() <= 64 &&
            mappingMethod.size() <= 32 && publicPort >= 0 && publicPort <= 65535 &&
            localPort >= 0 && localPort <= 65535 &&
            payload.value("acknowledgement").isBool()) {
            return Result<PacketPayload>::success(RendezvousMetadataPayload{
                secret, publicAddress, publicPort, localAddress, localPort, mappingMethod,
                payload.value("acknowledgement").toBool()});
        }
        break;
    }
    case PacketType::Ping:
    case PacketType::Pong:
        if (validUuid(payload.value("nonce")) && validTimestamp(payload.value("sent_at"))) {
            return Result<PacketPayload>::success(HeartbeatPayload{
                payload.value("nonce").toString(),
                QDateTime::fromString(payload.value("sent_at").toString(), Qt::ISODateWithMs)});
        }
        break;
    }
    return Result<PacketPayload>::failure("Invalid " + toString(type) + " payload");
}

} // namespace

Result<QByteArray> PacketCodec::encode(const Packet& packet) {
    if (!payloadMatchesType(packet.type, packet.payload)) {
        return Result<QByteArray>::failure("Packet type does not match its payload");
    }
    if (!isCanonicalUuid(packet.packetId) || !isCanonicalUuid(packet.meshId) ||
        !isCanonicalUuid(packet.senderId) || !packet.createdAt.isValid()) {
        return Result<QByteArray>::failure("Packet contains invalid fields");
    }
    if ((!packet.targetId.isEmpty() && !isCanonicalUuid(packet.targetId)) ||
        packet.ttl < 0 || packet.ttl > 16) {
        return Result<QByteArray>::failure("Packet contains invalid routing fields");
    }
    QJsonObject object{{"v", 0},
                       {"type", toString(packet.type)},
                       {"id", packet.packetId},
                       {"mesh", packet.meshId},
                       {"from", packet.senderId},
                       {"created_at", packet.createdAt.toUTC().toString(Qt::ISODateWithMs)},
                       {"body", payloadToJson(packet.payload)}};
    if (!packet.targetId.isEmpty()) {
        object.insert("to", packet.targetId);
    }
    if (packet.ttl > 0) {
        object.insert("ttl", packet.ttl);
    }
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxBytes) {
        return Result<QByteArray>::failure("Packet exceeds 64 KiB");
    }
    return Result<QByteArray>::success(std::move(bytes));
}

Result<Packet> PacketCodec::decode(const QByteArray& bytes, const QString& expectedMesh) {
    if (bytes.size() > MaxBytes) {
        return Result<Packet>::failure("Packet exceeds 64 KiB");
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<Packet>::failure("Invalid packet JSON");
    }
    const auto object = document.object();
    const auto type = packetTypeFromString(object.value("type").toString());
    if (object.value("v").toInt(-1) != 0 || !type) {
        return Result<Packet>::failure("Unsupported protocol version or packet type");
    }
    if (!validUuid(object.value("id")) || !validUuid(object.value("mesh")) ||
        !validUuid(object.value("from")) || !validTimestamp(object.value("created_at")) ||
        !object.value("body").isObject()) {
        return Result<Packet>::failure("Packet contains invalid fields");
    }
    const auto packetId = object.value("id").toString();
    const auto meshId = object.value("mesh").toString();
    const auto senderId = object.value("from").toString();
    const auto createdAt =
        QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs);
    const auto targetId = object.value("to").toString();
    const auto ttl = object.value("ttl").toInt(0);
    if ((!targetId.isEmpty() && !isCanonicalUuid(targetId)) || ttl < 0 || ttl > 16) {
        return Result<Packet>::failure("Packet contains invalid routing fields");
    }
    if (!expectedMesh.isEmpty() && meshId != expectedMesh) {
        return Result<Packet>::failure("Packet belongs to another mesh");
    }
    const auto payload = decodePayload(*type, object.value("body").toObject());
    if (!payload) {
        return Result<Packet>::failure(payload.error());
    }
    return Result<Packet>::success(
        {*type, packetId, meshId, senderId, createdAt, payload.value(), targetId, ttl});
}

} // namespace tmc
