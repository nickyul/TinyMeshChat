#include "tmc/identity/peer_identity.h"

#include "tmc/security/security.h"

namespace tmc {

bool PeerIdentity::isValid() const {
    return security::validKey(peerId) && !displayName.trimmed().isEmpty();
}

} // namespace tmc
