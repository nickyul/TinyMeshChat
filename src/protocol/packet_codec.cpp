#include "protocol/packet_codec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
using namespace tmc;

namespace {
bool validUuid(const QJsonValue& value) {
    return value.isString() && !QUuid::fromString(value.toString()).isNull();
}

bool validTimestamp(const QJsonValue& value) {
    return value.isString() && QDateTime::fromString(value.toString(), Qt::ISODateWithMs).isValid();
}

bool validIdentity(const QJsonValue& value) {
    if (!value.isObject())
        return false;
    const auto identity = value.toObject();
    const auto name = identity.value("display_name");
    return validUuid(identity.value("peer_id")) && validUuid(identity.value("device_id")) &&
           name.isString() && !name.toString().trimmed().isEmpty() &&
           name.toString().size() <= 128 && validTimestamp(identity.value("created_at"));
}

bool validMeshPayload(const QString& type, const QJsonObject& payload) {
    const auto phase = payload.value("phase").toString();
    const auto expectedPhase = type == "mesh.offer" ? "offer" : "answer";
    const auto sdp = payload.value("sdp");
    return validUuid(payload.value("connection_id")) && validUuid(payload.value("route_id")) &&
           payload.value("hop_count").isDouble() && payload.value("hop_count").toInt() >= 0 &&
           payload.value("hop_count").toInt() <= 16 && phase == expectedPhase &&
           validIdentity(payload.value("from_peer")) &&
           validUuid(payload.value("target_peer_id")) && sdp.isString() &&
           !sdp.toString().isEmpty() && sdp.toString().toUtf8().size() < PacketCodec::MaxBytes;
}

bool validPayload(const Packet& packet) {
    const auto& payload = packet.payload;
    if (packet.type == "peer.hello") {
        const auto name = payload.value("display_name");
        return name.isString() && !name.toString().trimmed().isEmpty() &&
               name.toString().size() <= 128 && validUuid(payload.value("device_id"));
    }
    if (packet.type == "peer.hello_ack")
        return payload.isEmpty();
    if (packet.type == "peer.list") {
        if (!payload.value("peers").isArray())
            return false;
        const auto peers = payload.value("peers").toArray();
        if (peers.isEmpty() || peers.size() > 16)
            return false;
        QSet<QString> unique;
        for (const auto& value : peers) {
            if (!validIdentity(value))
                return false;
            const auto id = value.toObject().value("peer_id").toString();
            if (unique.contains(id))
                return false;
            unique.insert(id);
        }
        return true;
    }
    if (packet.type == "chat.message") {
        const auto text = payload.value("text");
        return validUuid(payload.value("message_id")) && text.isString() &&
               !text.toString().trimmed().isEmpty() &&
               text.toString().size() <= PacketCodec::MaxTextChars &&
               payload.value("logical_clock").isDouble() &&
               payload.value("logical_clock").toInteger() > 0;
    }
    if (packet.type == "chat.ack")
        return validUuid(payload.value("message_id"));
    if (packet.type == "voice.state")
        return payload.value("joined").isBool() && payload.value("muted").isBool();
    if (packet.type == "mesh.offer" || packet.type == "mesh.answer")
        return validMeshPayload(packet.type, payload);
    if (packet.type == "ping" || packet.type == "pong")
        return validUuid(payload.value("nonce")) && validTimestamp(payload.value("sent_at"));
    return false;
}
} // namespace

const QSet<QString>& PacketCodec::knownTypes() {
    static const QSet<QString> s{"peer.hello", "peer.hello_ack", "peer.list",   "chat.message",
                                 "chat.ack",   "voice.state",    "mesh.offer",  "mesh.answer",
                                 "ping",       "pong"};
    return s;
}
QByteArray PacketCodec::encode(const Packet& p) {
    return QJsonDocument(
               QJsonObject{{"protocol_version", 3},
                           {"packet_type", p.type},
                           {"packet_id", p.packetId},
                           {"room_id", p.roomId},
                           {"sender_id", p.senderId},
                           {"created_at", p.createdAt.toUTC().toString(Qt::ISODateWithMs)},
                           {"payload", p.payload}})
        .toJson(QJsonDocument::Compact);
}
Result<Packet> PacketCodec::decode(const QByteArray& b, const QString& room,
                                   const QSet<QString>& senders) {
    if (b.size() > MaxBytes)
        return Result<Packet>::failure("Packet exceeds 64 KiB");
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(b, &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        return Result<Packet>::failure("Invalid packet JSON");
    auto o = d.object();
    if (o["protocol_version"].toInt() != 3)
        return Result<Packet>::failure("Unsupported protocol version");
    Packet p{o["packet_type"].toString(),
             o["packet_id"].toString(),
             o["room_id"].toString(),
             o["sender_id"].toString(),
             QDateTime::fromString(o["created_at"].toString(), Qt::ISODateWithMs),
             o["payload"].toObject()};
    if (!knownTypes().contains(p.type))
        return Result<Packet>::failure("Unknown packet type");
    if (QUuid::fromString(p.packetId).isNull() || QUuid::fromString(p.roomId).isNull() ||
        QUuid::fromString(p.senderId).isNull() || !p.createdAt.isValid() ||
        !o["payload"].isObject())
        return Result<Packet>::failure("Packet contains invalid fields");
    if (!room.isEmpty() && p.roomId != room)
        return Result<Packet>::failure("Packet belongs to another room");
    if (!senders.isEmpty() && !senders.contains(p.senderId))
        return Result<Packet>::failure("Packet sender is not a room member");
    if (!validPayload(p))
        return Result<Packet>::failure("Invalid " + p.type + " payload");
    return Result<Packet>::success(p);
}
