#pragma once

#include "tmc/core/result.h"
#include "tmc/identity/peer_identity.h"

namespace tmc {

class IdentityManager {
public:
    static Result<PeerIdentity> load(const QString& path);
    static Result<PeerIdentity> create(const QString& path, const QString& displayName);
    static Result<PeerIdentity> updateDisplayName(const QString& path, const PeerIdentity& identity,
                                                  const QString& displayName);
};

} // namespace tmc
