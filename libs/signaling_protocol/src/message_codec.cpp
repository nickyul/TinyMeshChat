#include "tmc/signaling_protocol/message_codec.h"

#include <QJsonArray>
#include "tmc/security/security.h"
#include <QSet>
#include <QUuid>

#include <initializer_list>

namespace tmc::signaling_protocol {
namespace {

// A joining peer receives at most five existing peers in the current wire format.
constexpr qsizetype MaxRoomPeers = 5;

CodecError invalidBody() {
    return {CodecErrorCode::InvalidBody, QStringLiteral("Missing or invalid message body field")};
}

bool fields(const QJsonObject& body, std::initializer_list<const char*> names) {
    if (body.size() != static_cast<qsizetype>(names.size())) {
        return false;
    }
    for (const auto* name : names) {
        if (!body.contains(QLatin1String(name))) {
            return false;
        }
    }
    return true;
}

bool uuid(const QJsonValue& value) {
    if (!value.isString()) {
        return false;
    }
    const auto text = value.toString();
    const auto id = QUuid::fromString(text);
    return !id.isNull() && id.toString(QUuid::WithoutBraces) == text;
}

bool identity(const QJsonValue& value) { return value.isString() && security::validKey(value.toString()); }

bool base64(const QJsonValue& value, qsizetype maxBytes, qsizetype exactBytes = 0) {
    if (!value.isString()) {
        return false;
    }
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > (maxBytes * 4 + 2) / 3) {
        return false;
    }
    const auto bytes = QByteArray::fromBase64(
        text.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    return !bytes.isEmpty() && bytes.size() <= maxBytes &&
           (exactBytes == 0 || bytes.size() == exactBytes) &&
           QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                             QByteArray::OmitTrailingEquals)) == text;
}

bool displayName(const QJsonValue& value) {
    if (!value.isString() || value.toString().trimmed().isEmpty() || value.toString().size() > 128) return false;
    for (const auto c : value.toString()) if (!c.isPrint()) return false;
    return true;
}

bool identifiers(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() > MaxAcquaintances) return false;
    QSet<QString> seen;
    for (const auto& item : value.toArray()) {
        if (!identity(item) || seen.contains(item.toString())) return false;
        seen.insert(item.toString());
    }
    return true;
}

bool outcome(const QJsonValue& value) {
    const auto status = value.toString();
    return status == "accepted" || status == "declined" || status == "busy" ||
           status == "expired" || status == "cancelled" || status == "unavailable";
}

bool errorBody(const QJsonObject& body) {
    if (!fields(body, {"code", "message"}) || !body.value("code").isString() ||
        !body.value("message").isString()) {
        return false;
    }
    const auto code = body.value("code").toString();
    const auto message = body.value("message").toString();
    if (code.isEmpty() || code.size() > 64 || message.isEmpty() || message.size() > 256) {
        return false;
    }
    for (const auto c : code) {
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') {
            return false;
        }
    }
    for (const auto c : message) {
        if (c.unicode() < 32 || c.unicode() > 126) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<CodecError> MessageCodec::validateRequest(const Envelope& envelope) {
    if (const auto failure = EnvelopeCodec::validateHeader(envelope)) {
        return failure;
    }
    if (!envelope.requestId) {
        return CodecError{CodecErrorCode::InvalidRequestId,
                          QStringLiteral("Request identifier is required")};
    }
    const auto& type = envelope.type;
    const auto& body = envelope.body;
    bool valid = false;
    if (type == "auth.authenticate") {
        valid = fields(body, {"publicKey", "grant", "signature"}) && identity(body.value("publicKey")) &&
                body.value("grant").isObject() && base64(body.value("signature"), 64, 64);
    } else if (type == "access.redeem") {
        valid = fields(body, {"publicKey", "token", "signature"}) && identity(body.value("publicKey")) &&
                base64(body.value("token"), 32, 32) && base64(body.value("signature"), 64, 64);
    } else if (type == "access.invite") {
        valid = body.isEmpty();
    } else if (type == "presence.publish") {
        valid = fields(body, {"identityId", "displayName", "knownPeers", "busy"}) &&
                identity(body.value("identityId")) && displayName(body.value("displayName")) &&
                identifiers(body.value("knownPeers")) && body.value("busy").isBool();
        for (const auto& peer : body.value("knownPeers").toArray())
            if (peer == body.value("identityId")) valid = false;
    } else if (type == "contact.invite") {
        valid = fields(body, {"toIdentityId", "roomId", "meshId"}) &&
                identity(body.value("toIdentityId")) && uuid(body.value("roomId")) && uuid(body.value("meshId"));
    } else if (type == "contact.respond") {
        const auto decision = body.value("decision").toString();
        valid = fields(body, {"invitationId", "decision"}) && uuid(body.value("invitationId")) &&
                (decision == "accept" || decision == "decline" || decision == "busy");
    } else if (type == "room.create") {
        if (!body.isEmpty()) {
            return CodecError{CodecErrorCode::UnknownField,
                              QStringLiteral("Room creation request body must be empty")};
        }
        valid = true;
    } else if (type == "invite.create" || type == "room.leave") {
        valid = fields(body, {"roomId"}) && uuid(body.value("roomId"));
    } else if (type == "room.join") {
        valid = fields(body, {"roomId", "token"}) && uuid(body.value("roomId")) &&
                base64(body.value("token"), 32, 32);
    } else if (type == "signal.send") {
        valid = fields(body, {"roomId", "toPeerId", "payload"}) &&
                uuid(body.value("roomId")) && uuid(body.value("toPeerId")) &&
                base64(body.value("payload"), MaxSignalBytes);
    } else {
        return CodecError{CodecErrorCode::UnknownType, QStringLiteral("Unknown request type")};
    }
    return valid ? std::nullopt : std::optional<CodecError>{invalidBody()};
}

std::optional<CodecError> MessageCodec::validateServerMessage(const Envelope& envelope) {
    if (const auto failure = EnvelopeCodec::validateHeader(envelope)) {
        return failure;
    }
    const auto& type = envelope.type;
    const auto& body = envelope.body;
    if (type == "error") {
        return errorBody(body) ? std::nullopt : std::optional<CodecError>{invalidBody()};
    }
    const bool event = type == "auth.challenge" || type == "peer.joined" ||
                       type == "peer.left" || type == "signal.received" ||
                       type == "presence.snapshot" || type == "contact.invitation" ||
                       type == "contact.accepted" || type == "contact.result" || type == "contact.closed";
    if (event == envelope.requestId.has_value()) {
        return CodecError{CodecErrorCode::InvalidRequestId,
                          QStringLiteral("Unexpected or missing request identifier")};
    }
    bool valid = false;
    if (type == "auth.challenge") {
        valid = fields(body, {"sessionId", "nonce", "authority"}) && uuid(body.value("sessionId")) &&
                base64(body.value("nonce"), 32, 32) && identity(body.value("authority"));
    } else if (type == "access.granted") {
        const auto grant = body.value("grant").toObject();
        valid = fields(body, {"grant"}) && security::verifyGrant(grant,
            grant.value("authority").toString(), grant.value("subject").toString());
    } else if (type == "access.invited") {
        valid = fields(body, {"token", "authority", "expiresInSeconds"}) && base64(body.value("token"), 32, 32) &&
                identity(body.value("authority")) && body.value("expiresInSeconds").toInt(-1) == InvitationLifetimeSeconds;
    } else if (type == "auth.authenticated" || type == "presence.published" || type == "contact.responded") {
        valid = body.isEmpty();
    } else if (type == "presence.snapshot") {
        valid = fields(body, {"peers"}) && body.value("peers").isArray() &&
                body.value("peers").toArray().size() <= MaxAcquaintances;
        QSet<QString> seen;
        for (const auto& item : body.value("peers").toArray()) {
            const auto peer = item.toObject();
            const auto id = peer.value("identityId").toString();
            if (!fields(peer, {"identityId", "online"}) || !identity(peer.value("identityId")) ||
                !peer.value("online").isBool() || seen.contains(id)) valid = false;
            seen.insert(id);
        }
    } else if (type == "contact.invited") {
        valid = fields(body, {"invitationId"}) && uuid(body.value("invitationId"));
    } else if (type == "contact.invitation") {
        valid = fields(body, {"invitationId", "fromIdentityId", "displayName", "meshId", "expiresInSeconds"}) &&
                uuid(body.value("invitationId")) && identity(body.value("fromIdentityId")) &&
                displayName(body.value("displayName")) && uuid(body.value("meshId")) &&
                body.value("expiresInSeconds").toInt(-1) == OnlineInvitationLifetimeSeconds;
    } else if (type == "contact.accepted") {
        valid = fields(body, {"invitationId", "roomId", "meshId", "token"}) &&
                uuid(body.value("invitationId")) && uuid(body.value("roomId")) &&
                uuid(body.value("meshId")) && base64(body.value("token"), 32, 32);
    } else if (type == "contact.result") {
        valid = fields(body, {"invitationId", "toIdentityId", "status"}) &&
                uuid(body.value("invitationId")) && identity(body.value("toIdentityId")) && outcome(body.value("status"));
    } else if (type == "contact.closed") {
        valid = fields(body, {"invitationId", "status"}) && uuid(body.value("invitationId")) && outcome(body.value("status"));
    } else if (type == "room.created") {
        for (auto it = body.constBegin(); it != body.constEnd(); ++it) {
            if (it.key() != "roomId" && it.key() != "peerId") {
                return CodecError{CodecErrorCode::UnknownField,
                                  QStringLiteral("Unknown room creation response field")};
            }
        }
        if (!uuid(body.value("roomId")) || !uuid(body.value("peerId"))) {
            return CodecError{CodecErrorCode::InvalidBody,
                              QStringLiteral("Room and peer identifiers must be canonical non-null UUIDs")};
        }
        valid = true;
    } else if (type == "invite.created") {
        valid = fields(body, {"roomId", "token", "expiresInSeconds"}) &&
                uuid(body.value("roomId")) && base64(body.value("token"), 32, 32) &&
                body.value("expiresInSeconds").isDouble() &&
                body.value("expiresInSeconds").toDouble() == InvitationLifetimeSeconds;
    } else if (type == "room.joined") {
        valid = fields(body, {"roomId", "peerId", "peers"}) &&
                uuid(body.value("roomId")) && uuid(body.value("peerId")) &&
                body.value("peers").isArray();
        const auto peers = body.value("peers").toArray();
        QSet<QString> seen;
        for (const auto& peer : peers) {
            if (!uuid(peer) || peer.toString() == body.value("peerId").toString() ||
                seen.contains(peer.toString())) {
                valid = false;
            }
            seen.insert(peer.toString());
        }
        valid = valid && peers.size() <= MaxRoomPeers;
    } else if (type == "room.left") {
        valid = fields(body, {"roomId"}) && uuid(body.value("roomId"));
    } else if (type == "signal.accepted") {
        valid = body.isEmpty();
    } else if (type == "peer.joined") {
        valid = fields(body, {"roomId", "peerId"}) &&
                uuid(body.value("roomId")) && uuid(body.value("peerId"));
    } else if (type == "peer.left") {
        const auto reason = body.value("reason").toString();
        valid = fields(body, {"roomId", "peerId", "reason"}) &&
                uuid(body.value("roomId")) && uuid(body.value("peerId")) &&
                (reason == "leave" || reason == "disconnect");
    } else if (type == "signal.received") {
        valid = fields(body, {"roomId", "fromPeerId", "payload"}) &&
                uuid(body.value("roomId")) && uuid(body.value("fromPeerId")) &&
                base64(body.value("payload"), MaxSignalBytes);
    } else {
        return CodecError{CodecErrorCode::UnknownType, QStringLiteral("Unknown server message type")};
    }
    return valid ? std::nullopt : std::optional<CodecError>{invalidBody()};
}

bool MessageCodec::isResponseFor(const QString& requestType, const QString& responseType) {
    static constexpr struct {
        const char* request;
        const char* response;
    } pairs[] = {
        {"auth.authenticate", "auth.authenticated"},
        {"access.redeem", "access.granted"},
        {"access.invite", "access.invited"},
        {"presence.publish", "presence.published"},
        {"contact.invite", "contact.invited"},
        {"contact.respond", "contact.responded"},
        {"room.create", "room.created"},
        {"invite.create", "invite.created"},
        {"room.join", "room.joined"},
        {"room.leave", "room.left"},
        {"signal.send", "signal.accepted"},
    };

    for (const auto& pair : pairs) {
        if (requestType == QLatin1String(pair.request)) {
            return responseType == QLatin1String(pair.response);
        }
    }
    return false;
}

Envelope MessageCodec::error(const std::optional<QString>& requestId, const QString& code) {
    // Codes carry machine-readable detail; the description never includes untrusted input.
    return {ProtocolVersion, QStringLiteral("error"), requestId,
            {{QStringLiteral("code"), code},
             {QStringLiteral("message"), QStringLiteral("Signaling request rejected")}}};
}

} // namespace tmc::signaling_protocol
