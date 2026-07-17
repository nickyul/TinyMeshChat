#include "tmc/app/mesh_coordinator.h"

#include <QDateTime>
#include <QJsonObject>
#include <QTimer>

namespace tmc {

namespace {

QJsonObject identityJson(const PeerIdentity& identity) {
    return {{"peer_id", identity.peerId},
            {"display_name", identity.displayName},
            {"device_id", identity.deviceId},
            {"created_at", identity.createdAt.toUTC().toString(Qt::ISODateWithMs)}};
}

PeerIdentity identityFromJson(const QJsonObject& object) {
    return {object.value("peer_id").toString(), object.value("display_name").toString(),
            object.value("device_id").toString(),
            QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs)};
}

} // namespace

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

bool MeshCoordinator::established() const {
    return established_;
}

int MeshCoordinator::peerCount() const {
    return peers_.size();
}

PeerIdentity MeshCoordinator::peer(const QString& peerId) const {
    return peers_.peer(peerId);
}

QList<PeerIdentity> MeshCoordinator::peers() const {
    return peers_.peers();
}

void MeshCoordinator::create(const PeerIdentity& localIdentity, const QString& meshId) {
    ++sessionGeneration_;
    resetRuntime();
    meshId_ = meshId;
    established_ = true;
    peers_.remember(localIdentity);
    updateState();
}

void MeshCoordinator::beginJoin(const PeerIdentity& localIdentity, const PeerIdentity& inviter,
                                const QString& meshId) {
    ++sessionGeneration_;
    resetRuntime();
    meshId_ = meshId;
    established_ = false;
    peers_.remember(localIdentity);
    peers_.remember(inviter);
    updateState();
}

void MeshCoordinator::leave() {
    ++sessionGeneration_;
    resetRuntime();
    meshId_.clear();
    established_ = false;
    updateState();
}

void MeshCoordinator::markEstablished() {
    established_ = true;
    updateState();
}

bool MeshCoordinator::rememberPeer(const PeerIdentity& peer) {
    if (meshId_.isEmpty()) {
        return false;
    }
    return peers_.remember(peer);
}

bool MeshCoordinator::ingestPeerList(const QJsonArray& peers, const QString& localPeerId) {
    bool changed = false;
    QSet<QString> knownIds;
    for (const auto& known : peers_.peers()) {
        knownIds.insert(known.peerId);
    }
    for (const auto& value : peers) {
        const auto candidate = identityFromJson(value.toObject());
        if (candidate.peerId == localPeerId) {
            continue;
        }
        if (!knownIds.contains(candidate.peerId) && knownIds.size() >= policy_.maxPeers) {
            continue;
        }
        if (peers_.remember(candidate)) {
            changed = true;
            knownIds.insert(candidate.peerId);
        }
    }
    return changed;
}

QJsonArray MeshCoordinator::peerList() const {
    QJsonArray result;
    for (const auto& peer : peers_.peers()) {
        result.append(identityJson(peer));
    }
    return result;
}

bool MeshCoordinator::rememberRoute(const QString& routeId) {
    if (seenRoutes_.contains(routeId)) {
        return false;
    }
    if (seenRoutes_.size() >= 1024) {
        seenRoutes_.clear();
    }
    seenRoutes_.insert(routeId);
    return true;
}

bool MeshCoordinator::shouldInitiateLink(const QString& localPeerId,
                                         const QString& remotePeerId) const {
    return localPeerId.compare(remotePeerId, Qt::CaseSensitive) < 0;
}

bool MeshCoordinator::canAttemptLink(const QString& peerId) const {
    return !retryScheduled_.contains(peerId) && !degradedPeers_.contains(peerId);
}

void MeshCoordinator::connectionOpened(const PeerIdentity& peer) {
    established_ = true;
    if (!peer.peerId.isEmpty()) {
        retryCounts_.remove(peer.peerId);
        retryScheduled_.remove(peer.peerId);
        degradedPeers_.remove(peer.peerId);
        ++retryGenerations_[peer.peerId];
    }
    updateState();
}

void MeshCoordinator::scheduleRetry(const PeerIdentity& peer) {
    if (peer.peerId.isEmpty() || meshId_.isEmpty() || retryScheduled_.contains(peer.peerId) ||
        degradedPeers_.contains(peer.peerId)) {
        return;
    }
    const int retryIndex = retryCounts_.value(peer.peerId);
    if (retryIndex >= static_cast<int>(policy_.meshRetryDelaysSeconds.size())) {
        degradedPeers_.insert(peer.peerId);
        updateState();
        emit statusChanged("Прямой канал с " + peer.displayName +
                           " недоступен после трёх повторных попыток. Mesh работает в "
                           "деградированном режиме.");
        return;
    }

    const int delay = policy_.meshRetryDelaysSeconds[retryIndex];
    retryCounts_.insert(peer.peerId, retryIndex + 1);
    retryScheduled_.insert(peer.peerId);
    const auto retryGeneration = ++retryGenerations_[peer.peerId];
    const auto sessionGeneration = sessionGeneration_;
    emit statusChanged(QString("Повторная попытка прямого канала с %1 через %2 с.")
                           .arg(peer.displayName)
                           .arg(delay));
    QTimer::singleShot(delay * 1000, this, [this, peer, retryGeneration, sessionGeneration] {
        if (sessionGeneration_ != sessionGeneration ||
            retryGenerations_.value(peer.peerId) != retryGeneration) {
            return;
        }
        retryScheduled_.remove(peer.peerId);
        emit retryRequested(peer);
    });
}

void MeshCoordinator::resetRuntime() {
    peers_.clear();
    seenRoutes_.clear();
    retryCounts_.clear();
    retryGenerations_.clear();
    retryScheduled_.clear();
    degradedPeers_.clear();
}

void MeshCoordinator::updateState() {
    if (meshId_.isEmpty()) {
        setState(MeshSessionState::Disconnected);
    } else if (!degradedPeers_.isEmpty()) {
        setState(MeshSessionState::Degraded);
    } else if (established_) {
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
