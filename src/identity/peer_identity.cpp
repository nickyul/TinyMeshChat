#include "tmc/identity/peer_identity.h"

#include "tmc/core/uuid.h"

namespace tmc {

bool PeerIdentity::isValid() const {
    return isCanonicalUuid(peerId) && !displayName.trimmed().isEmpty();
}

} // namespace tmc
