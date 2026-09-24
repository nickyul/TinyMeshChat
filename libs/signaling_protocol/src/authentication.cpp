#include "tmc/signaling_protocol/authentication.h"

#include "tmc/security/security.h"

namespace tmc::signaling_protocol {

QByteArray AuthenticationChallenge::authenticationMessage(const QString& publicKey) const {
    return security::transcript("tmc.ws-auth.v1", {sessionId, nonce, authority, publicKey});
}

QByteArray AuthenticationChallenge::redemptionMessage(const QString& publicKey,
                                                     const QString& token) const {
    return security::transcript("tmc.ws-redeem.v1", {sessionId, nonce, authority, publicKey, token});
}

} // namespace tmc::signaling_protocol
