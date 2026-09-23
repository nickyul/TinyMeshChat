#include "tmc/signaling_client/server_invitation.h"
#include "tmc/signaling_client/signaling_client.h"
#include "tmc/signaling_protocol/message_codec.h"

#include <QUuid>

namespace tmc {
using namespace signaling_protocol;

QString encodeServerInvitation(const ServerInvitation& invitation) {
    const Envelope envelope{1, QStringLiteral("mesh.invitation"), std::nullopt,
        {{"server", invitation.server.toString(QUrl::FullyEncoded)},
         {"roomId", invitation.roomId}, {"meshId", invitation.meshId}, {"token", invitation.token}}};
    const auto encoded = EnvelopeCodec::encode(envelope);
    const auto* bytes = std::get_if<QByteArray>(&encoded);
    if (!bytes) {
        return {};
    }
    const auto link = QStringLiteral("tinymesh://join/1#") + QString::fromLatin1(
        bytes->toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    return decodeServerInvitation(link) ? link : QString{};
}

std::optional<ServerInvitation> decodeServerInvitation(const QString& link) {
    if (link.size() > 8192 || !link.startsWith("tinymesh://join/1#")) {
        return std::nullopt;
    }
    const auto encoded = link.sliced(QStringLiteral("tinymesh://join/1#").size());
    const auto bytes = QByteArray::fromBase64(encoded.toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                         QByteArray::OmitTrailingEquals)) != encoded) {
        return std::nullopt;
    }
    const auto decoded = EnvelopeCodec::decode(bytes);
    const auto* envelope = std::get_if<Envelope>(&decoded);
    if (!envelope || envelope->type != "mesh.invitation" || envelope->requestId ||
        envelope->body.size() != 4) {
        return std::nullopt;
    }
    const auto& body = envelope->body;
    for (const auto* key : {"server", "roomId", "meshId", "token"}) {
        if (!body.value(QLatin1String(key)).isString()) {
            return std::nullopt;
        }
    }
    ServerInvitation result{QUrl(body.value("server").toString(), QUrl::StrictMode),
        body.value("roomId").toString(), body.value("meshId").toString(),
        body.value("token").toString()};
    const auto mesh = QUuid::fromString(result.meshId);
    if (!SignalingClient::validServerUrl(result.server) || mesh.isNull() ||
        mesh.toString(QUuid::WithoutBraces) != result.meshId ||
        MessageCodec::validateRequest({1, QStringLiteral("room.join"), QStringLiteral("check"),
                                      {{"roomId", result.roomId}, {"token", result.token}}})) {
        return std::nullopt;
    }
    return result;
}

} // namespace tmc
