#include "tmc/protocol/packet_codec.h"

#include <QJsonDocument>
#include <QUuid>

namespace tmc {

namespace {

bool validUuid(const QJsonValue& value) {
    return value.isString() && !QUuid::fromString(value.toString()).isNull();
}

bool validTimestamp(const QJsonValue& value) {
    return value.isString() && QDateTime::fromString(value.toString(), Qt::ISODateWithMs).isValid();
}

Result<PeerIdentity> decodeIdentity(const QJsonValue& value) {
    if (!value.isObject()) {
        return Result<PeerIdentity>::failure("Invalid peer identity");
    }
    const auto object = value.toObject();
    PeerIdentity identity{
        object.value("peer_id").toString(), object.value("display_name").toString(),
        object.value("device_id").toString(),
        QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs)};
    if (!identity.isValid() || identity.displayName.size() > 128) {
        return Result<PeerIdentity>::failure("Invalid peer identity");
    }
    return Result<PeerIdentity>::success(identity);
}

Result<PacketPayload> decodePayload(PacketType type, const QJsonObject& payload) {
    switch (type) {
    case PacketType::PeerHello: {
        const auto displayName = payload.value("display_name").toString();
        if (displayName.trimmed().isEmpty() || displayName.size() > 128 ||
            !validUuid(payload.value("device_id"))) {
            break;
        }
        return Result<PacketPayload>::success(
            HelloPayload{displayName, payload.value("device_id").toString()});
    }
    case PacketType::PeerHelloAck:
        if (payload.isEmpty()) {
            return Result<PacketPayload>::success(EmptyPayload{});
        }
        break;
    case PacketType::PeerList: {
        if (!payload.value("peers").isArray()) {
            break;
        }
        const auto peers = payload.value("peers").toArray();
        if (peers.isEmpty() || peers.size() > 16) {
            break;
        }
        QSet<QString> unique;
        for (const auto& peer : peers) {
            const auto decoded = decodeIdentity(peer);
            if (!decoded || unique.contains(decoded.value().peerId)) {
                return Result<PacketPayload>::failure("Invalid peer.list payload");
            }
            unique.insert(decoded.value().peerId);
        }
        return Result<PacketPayload>::success(PeerListPayload{peers});
    }
    case PacketType::ChatMessage: {
        const auto messageId = payload.value("message_id").toString();
        const auto text = payload.value("text").toString();
        const auto clock = payload.value("logical_clock").toInteger();
        if (QUuid::fromString(messageId).isNull() || text.trimmed().isEmpty() ||
            text.size() > PacketCodec::MaxTextChars || clock <= 0) {
            break;
        }
        return Result<PacketPayload>::success(ChatMessagePayload{messageId, text, clock});
    }
    case PacketType::ChatAck: {
        const auto messageId = payload.value("message_id").toString();
        if (!QUuid::fromString(messageId).isNull()) {
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
    case PacketType::MeshOffer:
    case PacketType::MeshAnswer: {
        const auto expectedPhase = type == PacketType::MeshOffer ? "offer" : "answer";
        const auto identity = decodeIdentity(payload.value("from_peer"));
        const auto hopCount = payload.value("hop_count").toInt(-1);
        const auto sdp = payload.value("sdp").toString();
        if (!identity || payload.value("phase").toString() != expectedPhase || hopCount < 0 ||
            hopCount > 16 || !validUuid(payload.value("route_id")) ||
            !validUuid(payload.value("connection_id")) ||
            !validUuid(payload.value("target_peer_id")) || sdp.isEmpty() ||
            sdp.toUtf8().size() >= PacketCodec::MaxBytes) {
            break;
        }
        return Result<PacketPayload>::success(
            MeshSignalingPayload{expectedPhase, payload.value("route_id").toString(), hopCount,
                                 payload.value("connection_id").toString(), identity.value(),
                                 payload.value("target_peer_id").toString(), sdp});
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

const QSet<QString>& PacketCodec::knownTypes() {
    static const QSet<QString> types{
        "peer.hello",  "peer.hello_ack", "peer.list",   "chat.message", "chat.ack",
        "voice.state", "mesh.offer",     "mesh.answer", "ping",         "pong"};
    return types;
}

QByteArray PacketCodec::encode(const Packet& packet) {
    return QJsonDocument(
               QJsonObject{{"protocol_version", 4},
                           {"packet_type", toString(packet.type)},
                           {"packet_id", packet.packetId},
                           {"mesh_id", packet.meshId},
                           {"sender_id", packet.senderId},
                           {"created_at", packet.createdAt.toUTC().toString(Qt::ISODateWithMs)},
                           {"payload", payloadToJson(packet.payload)}})
        .toJson(QJsonDocument::Compact);
}

Result<Packet> PacketCodec::decode(const QByteArray& bytes, const QString& expectedMesh,
                                   const QSet<QString>& allowedSenders) {
    if (bytes.size() > MaxBytes) {
        return Result<Packet>::failure("Packet exceeds 64 KiB");
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<Packet>::failure("Invalid packet JSON");
    }
    const auto object = document.object();
    const auto type = packetTypeFromString(object.value("packet_type").toString());
    if (object.value("protocol_version").toInt() != 4 || !type) {
        return Result<Packet>::failure("Unsupported protocol version or packet type");
    }
    if (!validUuid(object.value("packet_id")) || !validUuid(object.value("mesh_id")) ||
        !validUuid(object.value("sender_id")) || !validTimestamp(object.value("created_at")) ||
        !object.value("payload").isObject()) {
        return Result<Packet>::failure("Packet contains invalid fields");
    }
    Packet packet{*type,
                  object.value("packet_id").toString(),
                  object.value("mesh_id").toString(),
                  object.value("sender_id").toString(),
                  QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs),
                  EmptyPayload{}};
    if (!expectedMesh.isEmpty() && packet.meshId != expectedMesh) {
        return Result<Packet>::failure("Packet belongs to another mesh");
    }
    if (!allowedSenders.isEmpty() && !allowedSenders.contains(packet.senderId)) {
        return Result<Packet>::failure("Packet sender is not a mesh participant");
    }
    const auto payload = decodePayload(packet.type, object.value("payload").toObject());
    if (!payload) {
        return Result<Packet>::failure(payload.error());
    }
    packet.payload = payload.value();
    return Result<Packet>::success(packet);
}

} // namespace tmc
