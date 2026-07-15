#pragma once
#include "core/result.h"
#include "identity/peer_identity.h"
namespace tmc {
class IdentityManager {
  public:
    static Result<PeerIdentity> loadOrCreate(const QString& path, const QString& displayName);
};
} // namespace tmc
