#include "tmc/signaling/invitation_codec.h"

#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QtEndian>

namespace tmc {

namespace {

constexpr quint8 CompactVersion = 0;

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
    const auto id = QUuid::fromRfc4122(bytes);
    if (id.isNull()) {
        return false;
    }
    text = id.toString(QUuid::WithoutBraces);
    return true;
}

QByteArray encodeCompact(const Invitation& invitation) {
    QByteArray raw;
    QDataStream stream(&raw, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    const bool offer = invitation.kind == Invitation::Kind::Offer;
    stream << CompactVersion << static_cast<quint8>(offer ? 0 : 1);
    if ((offer && !writeUuid(stream, invitation.meshId)) ||
        !writeUuid(stream, invitation.connectionId)) {
        return {};
    }
    const auto sdp = invitation.sdp.toUtf8();
    stream << static_cast<quint32>(sdp.size());
    stream.writeRawData(sdp.constData(), sdp.size());
    return stream.status() == QDataStream::Ok ? raw : QByteArray{};
}

Result<Invitation> validate(Invitation invitation) {
    const bool offer = invitation.kind == Invitation::Kind::Offer;
    if ((offer && QUuid::fromString(invitation.meshId).isNull()) ||
        QUuid::fromString(invitation.connectionId).isNull() || invitation.sdp.isEmpty() ||
        invitation.sdp.toUtf8().size() > InvitationCodec::MaxBytes) {
        return Result<Invitation>::failure("Signaling payload has missing or invalid fields");
    }
    return Result<Invitation>::success(std::move(invitation));
}

Result<Invitation> decodeCompact(const QByteArray& raw) {
    QDataStream stream(raw);
    stream.setByteOrder(QDataStream::BigEndian);
    quint8 version{};
    quint8 kind{};
    stream >> version >> kind;
    if (version != CompactVersion || kind > 1) {
        return Result<Invitation>::failure("Invalid compact signaling header");
    }
    Invitation invitation;
    invitation.kind = kind == 0 ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    if ((invitation.kind == Invitation::Kind::Offer && !readUuid(stream, invitation.meshId)) ||
        !readUuid(stream, invitation.connectionId)) {
        return Result<Invitation>::failure("Invalid compact signaling identifiers");
    }
    quint32 sdpBytes{};
    stream >> sdpBytes;
    if (stream.status() != QDataStream::Ok || sdpBytes == 0 ||
        sdpBytes > static_cast<quint32>(InvitationCodec::MaxBytes) ||
        sdpBytes > static_cast<quint32>(raw.size())) {
        return Result<Invitation>::failure("Invalid compact signaling length");
    }
    QByteArray sdp(sdpBytes, Qt::Uninitialized);
    if (stream.readRawData(sdp.data(), sdp.size()) != sdp.size() || !stream.atEnd()) {
        return Result<Invitation>::failure("Truncated compact signaling payload");
    }
    invitation.sdp = QString::fromUtf8(sdp);
    return validate(std::move(invitation));
}

QString kindName(Invitation::Kind kind) {
    return kind == Invitation::Kind::Offer ? "offer" : "answer";
}

} // namespace

QByteArray InvitationCodec::encode(const Invitation& invitation) {
    QJsonObject object{{"v", 0},
                       {"type", kindName(invitation.kind)},
                       {"link", invitation.connectionId},
                       {"sdp", invitation.sdp}};
    if (invitation.kind == Invitation::Kind::Offer) {
        object.insert("mesh", invitation.meshId);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString InvitationCodec::encodeText(const Invitation& invitation) {
    const auto raw = encodeCompact(invitation);
    if (raw.isEmpty()) {
        return {};
    }
    return "tmc0:" + QString::fromLatin1(
                         qCompress(raw, 9).toBase64(QByteArray::Base64UrlEncoding |
                                                    QByteArray::OmitTrailingEquals));
}

QString InvitationCodec::encodeLink(const Invitation& invitation) {
    const auto text = encodeText(invitation);
    return text.isEmpty() ? QString{} : "tinymesh://signal/0/" + text.sliced(5);
}

Result<Invitation> InvitationCodec::decode(const QByteArray& bytes) {
    if (bytes.size() > MaxBytes) {
        return Result<Invitation>::failure("Signaling document is too large");
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<Invitation>::failure("Invalid signaling JSON: " + error.errorString());
    }
    const auto object = document.object();
    const auto type = object.value("type").toString();
    if (object.value("v").toInt(-1) != 0 || (type != "offer" && type != "answer")) {
        return Result<Invitation>::failure("Unsupported signaling version or type");
    }
    Invitation invitation;
    invitation.kind = type == "offer" ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    invitation.meshId = object.value("mesh").toString();
    invitation.connectionId = object.value("link").toString();
    invitation.sdp = object.value("sdp").toString();
    return validate(std::move(invitation));
}

Result<Invitation> InvitationCodec::decodeText(const QString& value) {
    auto text = value.trimmed();
    if (text.startsWith("tinymesh://signal/0/")) {
        text = "tmc0:" + text.sliced(QString("tinymesh://signal/0/").size());
    }
    if (!text.startsWith("tmc0:")) {
        return Result<Invitation>::failure("Text must start with tmc0: or tinymesh://signal/0/");
    }
    const auto encoded = QByteArray::fromBase64(text.sliced(5).toLatin1(),
                                                QByteArray::Base64UrlEncoding |
                                                    QByteArray::AbortOnBase64DecodingErrors);
    if (encoded.size() < 4 || encoded.size() > MaxBytes) {
        return Result<Invitation>::failure("Invalid or oversized Base64URL payload");
    }
    const auto expectedSize = qFromBigEndian<quint32>(encoded.constData());
    if (expectedSize == 0 || expectedSize > static_cast<quint32>(MaxBytes)) {
        return Result<Invitation>::failure("Compressed signaling payload is too large");
    }
    const auto raw = qUncompress(encoded);
    if (raw.isEmpty()) {
        return Result<Invitation>::failure("Invalid compressed signaling payload");
    }
    return decodeCompact(raw);
}

} // namespace tmc
