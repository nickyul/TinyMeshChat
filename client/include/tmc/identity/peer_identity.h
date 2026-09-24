#pragma once

#include <QString>

namespace tmc {

struct PeerIdentity {
    QString peerId;
    QString displayName;
    QString legacyPeerId; // Local migration metadata; never a verified network identity.

    bool isValid() const;
};

} // namespace tmc
