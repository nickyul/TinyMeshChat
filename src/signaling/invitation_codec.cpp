#include "tmc/signaling/invitation_codec.h"

#include "tmc/core/logger.h"

#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QtEndian>

namespace tmc {

namespace {

constexpr quint8 CompactVersion = 1;

bool writeUuid(QDataStream& stream, const QString& text) {
    const auto bytes = QUuid::fromString(text).toRfc4122();
    return bytes.size() == 16 &&
           stream.writeRawData(bytes.constData(), bytes.size()) == bytes.size();
}

bool readUuid(QDataStream& stream, QString& text) {
    QByteArray bytes(16, Qt::Uninitialized);
    if (stream.readRawData(bytes.data(), bytes.size()) != bytes.size()) {
        return false;
    }
    const auto uuid = QUuid::fromRfc4122(bytes);
    if (uuid.isNull()) {
        return false;
    }
    text = uuid.toString(QUuid::WithoutBraces);
    return true;
}

QByteArray encodeCompact(const Invitation& invitation) {
    QByteArray raw;
    QDataStream stream(&raw, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << CompactVersion
           << static_cast<quint8>(invitation.kind == Invitation::Kind::Offer ? 0 : 1);
    if (!writeUuid(stream, invitation.meshId) || !writeUuid(stream, invitation.connectionId) ||
        !writeUuid(stream, invitation.nonce) || !writeUuid(stream, invitation.fromPeer.peerId) ||
        !writeUuid(stream, invitation.fromPeer.deviceId)) {
        return {};
    }
    const auto displayName = invitation.fromPeer.displayName.toUtf8();
    const auto sdp = invitation.sdp.toUtf8();
    stream << invitation.createdAt.toSecsSinceEpoch()
           << static_cast<quint32>(invitation.createdAt.secsTo(invitation.expiresAt))
           << invitation.fromPeer.createdAt.toSecsSinceEpoch()
           << static_cast<quint16>(displayName.size()) << static_cast<quint32>(sdp.size());
    stream.writeRawData(displayName.constData(), displayName.size());
    stream.writeRawData(sdp.constData(), sdp.size());
    return stream.status() == QDataStream::Ok ? raw : QByteArray{};
}

Result<Invitation> decodeCompact(const QByteArray& raw, const QDateTime& now) {
    QDataStream stream(raw);
    stream.setByteOrder(QDataStream::BigEndian);
    quint8 version{};
    quint8 kind{};
    Invitation invitation;
    stream >> version >> kind;
    if (version != CompactVersion || kind > 1 || !readUuid(stream, invitation.meshId) ||
        !readUuid(stream, invitation.connectionId) || !readUuid(stream, invitation.nonce) ||
        !readUuid(stream, invitation.fromPeer.peerId) ||
        !readUuid(stream, invitation.fromPeer.deviceId)) {
        return Result<Invitation>::failure("Invalid compact signaling payload");
    }
    qint64 createdAtSeconds{};
    quint32 lifetimeSeconds{};
    qint64 peerCreatedAtSeconds{};
    quint16 displayNameBytes{};
    quint32 sdpBytes{};
    stream >> createdAtSeconds >> lifetimeSeconds >> peerCreatedAtSeconds >> displayNameBytes >>
        sdpBytes;
    if (stream.status() != QDataStream::Ok || displayNameBytes == 0 || displayNameBytes > 512 ||
        sdpBytes == 0 || sdpBytes > static_cast<quint32>(InvitationCodec::MaxBytes) ||
        lifetimeSeconds == 0 || lifetimeSeconds > 24 * 60 * 60 ||
        displayNameBytes + sdpBytes > static_cast<quint32>(raw.size())) {
        return Result<Invitation>::failure("Invalid compact signaling lengths");
    }
    QByteArray displayName(displayNameBytes, Qt::Uninitialized);
    QByteArray sdp(sdpBytes, Qt::Uninitialized);
    if (stream.readRawData(displayName.data(), displayName.size()) != displayName.size() ||
        stream.readRawData(sdp.data(), sdp.size()) != sdp.size() || !stream.atEnd()) {
        return Result<Invitation>::failure("Truncated compact signaling payload");
    }
    invitation.kind = kind == 0 ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    invitation.fromPeer.displayName = QString::fromUtf8(displayName);
    invitation.fromPeer.createdAt = QDateTime::fromSecsSinceEpoch(peerCreatedAtSeconds, Qt::UTC);
    invitation.sdp = QString::fromUtf8(sdp);
    invitation.createdAt = QDateTime::fromSecsSinceEpoch(createdAtSeconds, Qt::UTC);
    invitation.expiresAt = invitation.createdAt.addSecs(lifetimeSeconds);

    const auto document = InvitationCodec::encode(invitation);
    return InvitationCodec::decode(document, now);
}

} // namespace

static QString kindName(Invitation::Kind k) {
    return k == Invitation::Kind::Offer ? "offer" : "answer";
}

QByteArray InvitationCodec::encode(const Invitation& i) {
    QJsonObject p{{"peer_id", i.fromPeer.peerId},
                  {"display_name", i.fromPeer.displayName},
                  {"device_id", i.fromPeer.deviceId},
                  {"created_at", i.fromPeer.createdAt.toUTC().toString(Qt::ISODateWithMs)}};
    QJsonObject o{{"format", "tiny-mesh-signaling"},
                  {"format_version", 5},
                  {"kind", kindName(i.kind)},
                  {"mesh_id", i.meshId},
                  {"connection_id", i.connectionId},
                  {"from_peer", p},
                  {"sdp", i.sdp},
                  {"created_at", i.createdAt.toUTC().toString(Qt::ISODateWithMs)},
                  {"expires_at", i.expiresAt.toUTC().toString(Qt::ISODateWithMs)},
                  {"nonce", i.nonce}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QString InvitationCodec::encodeText(const Invitation& i) {
    const auto json = encode(i);
    const auto compact = encodeCompact(i);
    const auto jsonCompressed = qCompress(json, 9);
    if (compact.isEmpty()) {
        return "tmc3:" + QString::fromLatin1(jsonCompressed.toBase64(
                             QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }
    const auto compactCompressed = qCompress(compact, 9);
    Logger::instance().trace(
        "signaling",
        QString("json_bytes=%1 tmc3_compressed_bytes=%2 compact_bytes=%3 "
                "tmc4_compressed_bytes=%4 ratio=%5")
            .arg(json.size())
            .arg(jsonCompressed.size())
            .arg(compact.size())
            .arg(compactCompressed.size())
            .arg(compactCompressed.isEmpty()
                     ? 0.0
                     : static_cast<double>(jsonCompressed.size()) / compactCompressed.size(),
                 0, 'f', 2));
    return "tmc4:" + QString::fromLatin1(compactCompressed.toBase64(
                         QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString InvitationCodec::encodeLink(const Invitation& invitation) {
    const auto text = encodeText(invitation);
    return "tinymesh://signal/4/" + text.sliced(5);
}

Result<Invitation> InvitationCodec::decode(const QByteArray& b, const QDateTime& now) {
    if (b.size() > MaxBytes) {
        return Result<Invitation>::failure("Signaling document is too large");
    }
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(b, &e);
    if (e.error != QJsonParseError::NoError || !d.isObject()) {
        return Result<Invitation>::failure("Invalid signaling JSON: " + e.errorString());
    }
    auto o = d.object();
    if (o["format"].toString() != "tiny-mesh-signaling") {
        return Result<Invitation>::failure("Unknown signaling format");
    }
    if (o["format_version"].toInt() != 5) {
        return Result<Invitation>::failure("Unsupported signaling version");
    }
    const auto k = o["kind"].toString();
    if (k != "offer" && k != "answer") {
        return Result<Invitation>::failure("Invalid signaling kind");
    }
    auto p = o["from_peer"].toObject();
    Invitation i;
    i.kind = k == "offer" ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    i.meshId = o["mesh_id"].toString();
    i.connectionId = o["connection_id"].toString();
    i.sdp = o["sdp"].toString();
    i.nonce = o["nonce"].toString();
    i.createdAt = QDateTime::fromString(o["created_at"].toString(), Qt::ISODateWithMs);
    i.expiresAt = QDateTime::fromString(o["expires_at"].toString(), Qt::ISODateWithMs);
    const auto peerCreatedAt = QDateTime::fromString(p["created_at"].toString(), Qt::ISODateWithMs);
    i.fromPeer = {p["peer_id"].toString(), p["display_name"].toString(), p["device_id"].toString(),
                  peerCreatedAt.isValid() ? peerCreatedAt : i.createdAt};
    if (QUuid::fromString(i.meshId).isNull() || QUuid::fromString(i.connectionId).isNull() ||
        QUuid::fromString(i.nonce).isNull() || !i.fromPeer.isValid() ||
        i.fromPeer.displayName.size() > 128 || i.sdp.isEmpty() || !i.createdAt.isValid() ||
        !i.expiresAt.isValid() || i.expiresAt <= i.createdAt || i.createdAt > now.addSecs(300)) {
        return Result<Invitation>::failure("Signaling document has missing or invalid fields");
    }
    if (i.isExpired(now)) {
        return Result<Invitation>::failure("Signaling document has expired");
    }
    return Result<Invitation>::success(i);
}

Result<Invitation> InvitationCodec::decodeText(const QString& s, const QDateTime& now) {
    auto text = s.trimmed();
    if (text.startsWith("tinymesh://signal/4/")) {
        text = "tmc4:" + text.sliced(QString("tinymesh://signal/4/").size());
    } else if (text.startsWith("tinymesh://signal/")) {
        text = "tmc3:" + text.sliced(QString("tinymesh://signal/").size());
    }
    if (!text.startsWith("tmc2:") && !text.startsWith("tmc3:") && !text.startsWith("tmc4:")) {
        return Result<Invitation>::failure(
            "Text must start with tmc2:, tmc3:, tmc4: or tinymesh://");
    }
    const auto encoded = QByteArray::fromBase64(text.sliced(5).toLatin1(),
                                                QByteArray::Base64UrlEncoding |
                                                    QByteArray::AbortOnBase64DecodingErrors);
    if (encoded.isEmpty() || encoded.size() > MaxBytes) {
        return Result<Invitation>::failure("Invalid or oversized Base64URL payload");
    }
    QByteArray raw = encoded;
    if (text.startsWith("tmc3:") || text.startsWith("tmc4:")) {
        if (encoded.size() < 4) {
            return Result<Invitation>::failure("Invalid compressed signaling payload");
        }
        const auto expectedSize = qFromBigEndian<quint32>(encoded.constData());
        if (expectedSize == 0 || expectedSize > static_cast<quint32>(MaxBytes)) {
            return Result<Invitation>::failure("Compressed signaling payload is too large");
        }
        raw = qUncompress(encoded);
    }
    if (raw.isEmpty()) {
        return Result<Invitation>::failure("Invalid signaling payload");
    }
    if (text.startsWith("tmc4:")) {
        return decodeCompact(raw, now);
    }
    return decode(raw, now);
}

} // namespace tmc
