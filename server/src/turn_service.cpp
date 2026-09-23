#include "tmc/server/turn_service.h"

#include <QDateTime>
#include <QMessageAuthenticationCode>

#include <utility>

namespace tmc::server {

TurnService::TurnService(TurnSettings settings) : settings_(std::move(settings)) {}

signaling_protocol::TurnCredentials TurnService::issue(const QString& identityId) const {
    if (settings_.urls.isEmpty()) {
        return {};
    }
    // coturn TURN REST authentication: expiry:identity and base64(HMAC-SHA1(secret, username)).
    const auto expiresAt = QDateTime::currentSecsSinceEpoch() + settings_.lifetimeSeconds;
    const auto username = QString::number(expiresAt) + ':' + identityId;
    const auto password = QMessageAuthenticationCode::hash(
        username.toUtf8(), settings_.sharedSecret, QCryptographicHash::Sha1).toBase64();
    return {settings_.urls, username, QString::fromLatin1(password), settings_.lifetimeSeconds};
}

} // namespace tmc::server
