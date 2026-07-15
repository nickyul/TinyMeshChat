#include "signaling/invitation_codec.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
using namespace tmc;
static QString kindName(Invitation::Kind k) {
    return k == Invitation::Kind::Offer ? "offer" : "answer";
}
QByteArray InvitationCodec::encode(const Invitation& i) {
    QJsonObject p{{"peer_id", i.fromPeer.peerId},
                  {"display_name", i.fromPeer.displayName},
                  {"device_id", i.fromPeer.deviceId}};
    QJsonObject o{{"format", "tiny-mesh-signaling"},
                  {"format_version", 2},
                  {"kind", kindName(i.kind)},
                  {"room_id", i.roomId},
                  {"room_name", i.roomName},
                  {"connection_id", i.connectionId},
                  {"from_peer", p},
                  {"sdp", i.sdp},
                  {"created_at", i.createdAt.toUTC().toString(Qt::ISODateWithMs)},
                  {"expires_at", i.expiresAt.toUTC().toString(Qt::ISODateWithMs)},
                  {"nonce", i.nonce}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
QString InvitationCodec::encodeText(const Invitation& i) {
    return "tmc1:" + QString::fromLatin1(encode(i).toBase64(QByteArray::Base64UrlEncoding |
                                                            QByteArray::OmitTrailingEquals));
}
Result<Invitation> InvitationCodec::decode(const QByteArray& b, const QDateTime& now) {
    if (b.size() > MaxBytes)
        return Result<Invitation>::failure("Signaling document is too large");
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(b, &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        return Result<Invitation>::failure("Invalid signaling JSON: " + e.errorString());
    auto o = d.object();
    if (o["format"].toString() != "tiny-mesh-signaling")
        return Result<Invitation>::failure("Unknown signaling format");
    if (o["format_version"].toInt() != 2)
        return Result<Invitation>::failure("Unsupported signaling version");
    const auto k = o["kind"].toString();
    if (k != "offer" && k != "answer")
        return Result<Invitation>::failure("Invalid signaling kind");
    auto p = o["from_peer"].toObject();
    Invitation i;
    i.kind = k == "offer" ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    i.roomId = o["room_id"].toString();
    i.roomName = o["room_name"].toString().trimmed();
    i.connectionId = o["connection_id"].toString();
    i.sdp = o["sdp"].toString();
    i.nonce = o["nonce"].toString();
    i.createdAt = QDateTime::fromString(o["created_at"].toString(), Qt::ISODateWithMs);
    i.expiresAt = QDateTime::fromString(o["expires_at"].toString(), Qt::ISODateWithMs);
    i.fromPeer = {p["peer_id"].toString(), p["display_name"].toString(), p["device_id"].toString(),
                  i.createdAt};
    if (QUuid::fromString(i.roomId).isNull() || QUuid::fromString(i.connectionId).isNull() ||
        QUuid::fromString(i.nonce).isNull() || i.roomName.isEmpty() || i.roomName.size() > 128 ||
        !i.fromPeer.isValid() || i.fromPeer.displayName.size() > 128 || i.sdp.isEmpty() ||
        !i.createdAt.isValid() || !i.expiresAt.isValid() || i.expiresAt <= i.createdAt ||
        i.createdAt > now.addSecs(300))
        return Result<Invitation>::failure("Signaling document has missing or invalid fields");
    if (i.isExpired(now))
        return Result<Invitation>::failure("Signaling document has expired");
    return Result<Invitation>::success(i);
}
Result<Invitation> InvitationCodec::decodeText(const QString& s, const QDateTime& now) {
    if (!s.startsWith("tmc1:"))
        return Result<Invitation>::failure("Text must start with tmc1:");
    auto raw =
        QByteArray::fromBase64(s.sliced(5).toLatin1(), QByteArray::Base64UrlEncoding |
                                                           QByteArray::AbortOnBase64DecodingErrors);
    if (raw.isEmpty())
        return Result<Invitation>::failure("Invalid Base64URL payload");
    return decode(raw, now);
}
