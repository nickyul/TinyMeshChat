#include "tmc/signaling_client/access_invitation.h"
#include "tmc/signaling_client/signaling_client.h"
#include "tmc/signaling_protocol/envelope_codec.h"
#include <QJsonDocument>

namespace tmc {
QString encodeAccessInvitation(const AccessInvitation& invitation) {
    if (!SignalingClient::validServerUrl(invitation.server) || !security::validKey(invitation.authority) ||
        security::unbase64(invitation.token, 32).isEmpty()) return {};
    const auto encoded = signaling_protocol::EnvelopeCodec::encode({1, "access.invitation", std::nullopt,
        {{"server", invitation.server.toString(QUrl::FullyEncoded)}, {"authority", invitation.authority}, {"token", invitation.token}}});
    const auto* bytes = std::get_if<QByteArray>(&encoded);
    return bytes ? "tinymesh://access/1#" + security::base64(*bytes) : QString{};
}
std::optional<AccessInvitation> decodeAccessInvitation(const QString& text) {
    const QString prefix = "tinymesh://access/1#";
    if (!text.startsWith(prefix) || text.size() > 8192) return std::nullopt;
    const auto decoded = signaling_protocol::EnvelopeCodec::decode(security::unbase64(text.sliced(prefix.size())));
    const auto* value = std::get_if<signaling_protocol::Envelope>(&decoded);
    if (!value || value->type != "access.invitation" || value->requestId || value->body.size() != 3) return std::nullopt;
    AccessInvitation invitation{QUrl(value->body.value("server").toString(), QUrl::StrictMode),
        value->body.value("authority").toString(), value->body.value("token").toString()};
    if (encodeAccessInvitation(invitation).isEmpty()) return std::nullopt;
    return invitation;
}
QJsonObject decodeAccessGrant(const QString& text) {
    const QString prefix = "tmc-access1:";
    if (!text.startsWith(prefix) || text.size() > 8192) return {};
    const auto grant = QJsonDocument::fromJson(security::unbase64(text.sliced(prefix.size()))).object();
    return security::verifyGrant(grant, grant.value("authority").toString(), grant.value("subject").toString()) ? grant : QJsonObject{};
}
}
