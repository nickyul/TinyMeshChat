#include "tmc/app/mesh_coordinator.h"

#include <QRandomGenerator>
#include <QTimer>

namespace tmc {

MeshCoordinator::MeshCoordinator(ConnectionPolicy policy, QObject* parent)
    : QObject(parent), policy_(policy) {
    Q_ASSERT(policy_.isValid());
}

QString MeshCoordinator::meshId() const {
    return meshId_;
}

MeshSessionState MeshCoordinator::state() const {
    return state_;
}

bool MeshCoordinator::joined() const {
    return joined_;
}

int MeshCoordinator::peerCount() const {
    return peers_.size();
}

PeerIdentity MeshCoordinator::peer(const QString& peerId) const {
    return peers_.value(peerId);
}

QList<PeerIdentity> MeshCoordinator::peers() const {
    return peers_.values();
}

void MeshCoordinator::create(const PeerIdentity& localIdentity, const QString& meshId) {
    resetRuntime();
    meshId_ = meshId;
    joined_ = true;
    rememberPeer(localIdentity);
    updateState();
}

void MeshCoordinator::beginJoin(const PeerIdentity& localIdentity, const QString& meshId) {
    resetRuntime();
    meshId_ = meshId;
    joined_ = false;
    rememberPeer(localIdentity);
    updateState();
}

void MeshCoordinator::leave() {
    resetRuntime();
    meshId_.clear();
    joined_ = false;
    updateState();
}

bool MeshCoordinator::rememberPeer(const PeerIdentity& peer) {
    if (meshId_.isEmpty() || !peer.isValid()) {
        return false;
    }
    const auto existing = peers_.find(peer.peerId);
    if (existing == peers_.end()) {
        if (peers_.size() >= policy_.maxPeers) {
            return false;
        }
        peers_.insert(peer.peerId, peer);
        return true;
    }
    if (existing->displayName == peer.displayName) {
        return false;
    }
    *existing = peer;
    return true;
}

bool MeshCoordinator::forgetPeer(const QString& peerId) {
    if (peers_.remove(peerId) == 0) {
        return false;
    }
    retryStates_.remove(peerId);
    linkGenerations_.remove(peerId);
    updateState();
    return true;
}

bool MeshCoordinator::ingestPeerList(const QList<PeerIdentity>& peers, const QString& localPeerId) {
    bool changed = false;
    for (const auto& candidate : peers) {
        if (candidate.peerId == localPeerId) {
            continue;
        }
        if (rememberPeer(candidate)) {
            changed = true;
        }
    }
    return changed;
}

bool MeshCoordinator::canAttemptLink(const QString& peerId) const {
    if (meshId_.isEmpty() || !peers_.contains(peerId)) {
        return false;
    }
    const auto retry = retryStates_.constFind(peerId);
    return retry == retryStates_.cend() || !retry->scheduled;
}

void MeshCoordinator::connectionOpened(const PeerIdentity& peer) {
    if (!peer.isValid()) {
        return;
    }
    joined_ = true;
    retryStates_.remove(peer.peerId);
    updateState();
}

void MeshCoordinator::scheduleRetry(const PeerIdentity& peer) {
    if (meshId_.isEmpty() || !peer.isValid() || !peers_.contains(peer.peerId)) {
        return;
    }
    auto& retry = retryStates_[peer.peerId];
    if (retry.scheduled) {
        return;
    }

    int delay{};
    if (retry.attempts >= static_cast<int>(policy_.meshRetryDelaysSeconds.size())) {
        retry.degraded = true;
        updateState();
        delay = QRandomGenerator::global()->bounded(policy_.degradedRetryMinSeconds,
                                                     policy_.degradedRetryMaxSeconds + 1);
    } else {
        delay = policy_.meshRetryDelaysSeconds[retry.attempts];
    }

    ++retry.attempts;
    retry.scheduled = true;
    retry.token = ++nextRetryToken_;
    const auto token = retry.token;
    emit statusChanged(QString("Повторная попытка прямого канала с %1 через %2 с.")
                           .arg(peer.displayName)
                           .arg(delay));
    QTimer::singleShot(delay * 1000, this, [this, peer, token] {
        auto retry = retryStates_.find(peer.peerId);
        if (retry == retryStates_.end() || retry->token != token) {
            return;
        }
        retry->scheduled = false;
        emit retryRequested(peer);
    });
}

quint64 MeshCoordinator::nextLinkGeneration(const QString& peerId) {
    return ++linkGenerations_[peerId];
}

bool MeshCoordinator::acceptLinkGeneration(const QString& peerId, quint64 generation) {
    if (peerId.isEmpty() || generation == 0 || generation <= linkGenerations_.value(peerId)) {
        return false;
    }
    linkGenerations_.insert(peerId, generation);
    return true;
}

void MeshCoordinator::resetRetryBackoff() {
    retryStates_.clear();
    updateState();
}

void MeshCoordinator::routeAvailable(const QString& peerId) {
    const auto retry = retryStates_.find(peerId);
    if (retry == retryStates_.end() || !retry->degraded) {
        return;
    }
    retryStates_.erase(retry);
    updateState();
}

void MeshCoordinator::resetRuntime() {
    peers_.clear();
    retryStates_.clear();
    linkGenerations_.clear();
}

void MeshCoordinator::updateState() {
    bool hasDegradedPeer = false;
    for (const auto& retry : retryStates_) {
        if (retry.degraded) {
            hasDegradedPeer = true;
            break;
        }
    }

    if (meshId_.isEmpty()) {
        setState(MeshSessionState::Disconnected);
    } else if (hasDegradedPeer) {
        setState(MeshSessionState::Degraded);
    } else if (joined_) {
        setState(MeshSessionState::InMesh);
    } else {
        setState(MeshSessionState::Connecting);
    }
}

void MeshCoordinator::setState(MeshSessionState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state_);
}

} // namespace tmc
