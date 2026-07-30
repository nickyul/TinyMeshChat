#pragma once

#include <QString>

namespace tmc {

struct PeerIdentity {
    QString peerId;
    QString displayName;

    bool isValid() const;
};

} // namespace tmc
