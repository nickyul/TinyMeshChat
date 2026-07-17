#pragma once

#include "tmc/identity/peer_identity.h"

#include <QHash>
#include <QList>

namespace tmc {

class PeerRegistry {
public:
    bool remember(const PeerIdentity& peer);
    void clear();

    bool contains(const QString& peerId) const;
    int size() const;
    PeerIdentity peer(const QString& peerId) const;
    QList<PeerIdentity> peers() const;

private:
    QHash<QString, PeerIdentity> peers_;
};

} // namespace tmc
