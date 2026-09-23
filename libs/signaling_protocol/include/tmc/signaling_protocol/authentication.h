#pragma once

#include <QByteArray>
#include <QString>

namespace tmc::signaling_protocol {

struct AuthenticationChallenge {
    QString sessionId;
    QString nonce;
    QString authority;

    [[nodiscard]] QByteArray authenticationMessage(const QString& publicKey) const;
    [[nodiscard]] QByteArray redemptionMessage(const QString& publicKey, const QString& token) const;
};

} // namespace tmc::signaling_protocol
