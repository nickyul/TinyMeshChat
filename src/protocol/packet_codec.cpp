#include "tmc/protocol/packet_codec.h"

#include <QJsonDocument>
#include <QUuid>

namespace tmc {

namespace {

bool validUuid(const QJsonValue& value) {
    if (!value.isString()) {
        return false;
    }
    const auto text = value.toString();
    const auto uuid = QUuid::fromString(text);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == text;
}

bool validTimestamp(const QJsonValue& value) {
    return value.isString() && QDateTime::fromString(value.toString(), Qt::ISODateWithMs).isValid();
}

Result<PeerIdentity> decodeIdentity(const QJsonValue& value) {
    if (!value.isObject()) {
        return Result<PeerIdentity>::failure("Invalid peer identity");
    }
    const auto object = value.toObject();
    PeerIdentity identity{object.value("id").toString(), object.value("name").toString()};
    if (!identity.isValid() || identity.displayName.size() > 128) {
        return Result<PeerIdentity>::failure("Invalid peer identity");
    }
    return Result<PeerIdentity>::success(identity);
}

Result<PacketPayload> decodePayload(PacketType type, const QJsonObject& payload) {
    switch (type) {
    case PacketType::PeerHello: {
        const auto displayName = payload.value("display_name").toString();
        if (displayName.trimmed().isEmpty() || displayName.size() > 128) {
            break;
        }
        return Result<PacketPayload>::success(HelloPayload{displayName});
    }
    case PacketType::PeerSnapshot: {
        if (!payload.value("peers").isArray()) {
            break;
        }
        const auto peers = payload.value("peers").toArray();
        const auto revision = payload.value("revision").toInteger(-1);
        if (peers.isEmpty() || peers.size() > 16 || revision < 0) {
            break;
        }
        QSet<QString> unique;
        for (const auto& peer : peers) {
            const auto decoded = decodeIdentity(peer);
            if (!decoded || unique.contains(decoded.value().peerId)) {
                return Result<PacketPayload>::failure("Invalid peer.snapshot payload");
            }
            unique.insert(decoded.value().peerId);
        }
        return Result<PacketPayload>::success(PeerSnapshotPayload{revision, peers});
    }
    case PacketType::PeerAnnounce: {
        const auto peer = decodeIdentity(payload.value("peer"));
        const auto epoch = payload.value("epoch").toInteger(-1);
        const auto hops = payload.value("hops").toInt(-1);
        if (peer && epoch >= 0 && hops >= 0 && hops <= 16) {
            return Result<PacketPayload>::success(PeerAnnouncePayload{peer.value(), epoch, hops});
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
        if (!QUuid::fromString(requestId).isNull() && hops >= 0 && hops <= 16) {
            return Result<PacketPayload>::success(RoutePayload{requestId, hops});
        }
        break;
    }
    case PacketType::LinkOffer:
    case PacketType::LinkAnswer: {
        const auto connectionId = payload.value("link").toString();
        const auto generation = payload.value("generation").toInteger(-1);
        const auto sdp = payload.value("sdp").toString();
        if (QUuid::fromString(connectionId).isNull() || generation < 0 || sdp.isEmpty() ||
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
        const auto reason = payload.value("reason").toString();
        const auto sdp = payload.value("sdp").toString();
        if (!QUuid::fromString(connectionId).isNull() && negotiation >= 0 && reason.size() <= 64 &&
            !sdp.isEmpty() &&
            sdp.toUtf8().size() < PacketCodec::MaxBytes) {
            return Result<PacketPayload>::success(SessionSignalingPayload{
                connectionId, static_cast<quint64>(negotiation), reason, sdp});
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

const QSet<QString>& PacketCodec::knownTypes() {
    static const QSet<QString> types{
        "peer.hello",     "peer.snapshot",  "peer.announce",
        "peer.leave",     "chat.message",   "chat.ack",     "voice.state",
        "voice.quality",
        "route.request",  "route.reply",    "link.offer",   "link.answer",
        "session.offer",  "session.answer", "ping",         "pong"};
    return types;
}

QByteArray PacketCodec::encode(const Packet& packet) {
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
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
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
    const auto type = packetTypeFromString(object.value("type").toString());
    if (object.value("v").toInt(-1) != 0 || !type) {
        return Result<Packet>::failure("Unsupported protocol version or packet type");
    }
    if (!validUuid(object.value("id")) || !validUuid(object.value("mesh")) ||
        !validUuid(object.value("from")) || !validTimestamp(object.value("created_at")) ||
        !object.value("body").isObject()) {
        return Result<Packet>::failure("Packet contains invalid fields");
    }
    Packet packet{*type,
                  object.value("id").toString(),
                  object.value("mesh").toString(),
                  object.value("from").toString(),
                  QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs),
                  EmptyPayload{},
                  object.value("to").toString(),
                  object.value("ttl").toInt(0)};
    if ((!packet.targetId.isEmpty() && QUuid::fromString(packet.targetId).isNull()) ||
        packet.ttl < 0 || packet.ttl > 16) {
        return Result<Packet>::failure("Packet contains invalid routing fields");
    }
    if (!expectedMesh.isEmpty() && packet.meshId != expectedMesh) {
        return Result<Packet>::failure("Packet belongs to another mesh");
    }
    if (!allowedSenders.isEmpty() && !allowedSenders.contains(packet.senderId)) {
        return Result<Packet>::failure("Packet sender is not a mesh participant");
    }
    const auto payload = decodePayload(packet.type, object.value("body").toObject());
    if (!payload) {
        return Result<Packet>::failure(payload.error());
    }
    packet.payload = payload.value();
    return Result<Packet>::success(packet);
}

} // namespace tmc
