#include "signaling_server.h"
#include "tmc/signaling_protocol/message_codec.h"
#include <QWebSocket>

namespace tmc::server {
using namespace signaling_protocol;
bool SignalingServer::handleAccess(QWebSocket* socket, const Envelope& request) {
    const auto& type = request.type;
    if (!type.startsWith("auth.") && !type.startsWith("access.")) return false;
    auto session = clients_.find(socket);
    if (session == clients_.end()) return true;
    const auto reject = [&](const QString& code, bool close) {
        send(socket, MessageCodec::error(request.requestId, code));
        if (close) closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated, "Access rejected");
    };
    if (type == "access.invite") {
        if (session->identityId.isEmpty()) { reject("authentication_required", true); return true; }
        int count = 0;
        for (const auto& invite : accessInvitations_) if (invite.issuerSession == session->id) ++count;
        if (count >= 8 || accessInvitations_.size() >= 128) { reject("resource_limit", false); return true; }
        const auto token = security::randomToken();
        accessInvitations_.insert(token, {session->id, clock_.elapsed() + InvitationLifetimeSeconds * qint64{1000}});
        send(socket, {1, "access.invited", request.requestId,
            {{"token", token}, {"authority", authority_->publicKey()}, {"expiresInSeconds", InvitationLifetimeSeconds}}});
        return true;
    }
    if (!session->identityId.isEmpty() || session->nonce.isEmpty() || clock_.elapsed() - session->openedAt >= 15000) {
        reject("authentication_failed", true); return true;
    }
    const auto nonce = session->nonce;
    session->nonce.clear(); // A proof is valid once and only on this socket.
    const auto publicKey = request.body.value("publicKey").toString();
    QStringList fields{session->id, nonce, authority_->publicKey(), publicKey};
    QJsonObject grant;
    if (type == "auth.authenticate") {
        grant = request.body.value("grant").toObject();
        if (!security::verifyGrant(grant, authority_->publicKey(), publicKey) ||
            !security::verify(publicKey, security::transcript("tmc.ws-auth.v1", fields), request.body.value("signature").toString())) {
            reject("authentication_failed", true); return true;
        }
    } else if (type == "access.redeem") {
        const auto token = request.body.value("token").toString();
        fields.append(token);
        const auto invitation = accessInvitations_.constFind(token);
        if (invitation == accessInvitations_.cend() || clock_.elapsed() >= invitation->expiresAt ||
            !security::verify(publicKey, security::transcript("tmc.ws-redeem.v1", fields), request.body.value("signature").toString())) {
            reject("access_invitation_invalid", true); return true;
        }
        accessInvitations_.remove(token);
        grant = security::issueGrant(*authority_, publicKey);
    } else { reject("authentication_failed", true); return true; }
    session->identityId = security::identityId(publicKey);
    const auto sessionId = session->id;
    QVector<Delivery> deliveries;
    deliveries.append({sessionId, type == "access.redeem"
        ? Envelope{1, "access.granted", request.requestId, {{"grant", grant}}}
        : Envelope{1, "auth.authenticated", request.requestId, {}}});
    deliveries.append({sessionId, {1, "session.ready", std::nullopt, {{"sessionId", sessionId}}}});
    deliver(deliveries);
    return true;
}
} // namespace tmc::server
