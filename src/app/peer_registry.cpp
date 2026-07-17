#include "tmc/app/peer_registry.h"

namespace tmc {

bool PeerRegistry::remember(const PeerIdentity& peer) {
    if (peer.peerId.isEmpty()) {
        return false;
    }
    const bool added = !peers_.contains(peer.peerId);
    auto normalized = peer;
    if (normalized.displayName.isEmpty()) {
        normalized.displayName = normalized.peerId.left(8);
    }
    if (normalized.deviceId.isEmpty()) {
        normalized.deviceId = "unknown";
    }
    peers_.insert(normalized.peerId, normalized);
    return added;
}

void PeerRegistry::clear() {
    peers_.clear();
}

bool PeerRegistry::contains(const QString& peerId) const {
    return peers_.contains(peerId);
}

int PeerRegistry::size() const {
    return peers_.size();
}

PeerIdentity PeerRegistry::peer(const QString& peerId) const {
    return peers_.value(peerId);
}

QList<PeerIdentity> PeerRegistry::peers() const {
    return peers_.values();
}

} // namespace tmc
