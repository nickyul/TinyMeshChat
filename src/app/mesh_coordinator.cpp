#include "tmc/app/mesh_coordinator.h"

#include <QDateTime>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>

namespace tmc {

namespace {

QJsonObject identityJson(const PeerIdentity& identity) {
    return {{"id", identity.peerId},
            {"name", identity.displayName},
            {"device", identity.deviceId},
            {"created_at", identity.createdAt.toUTC().toString(Qt::ISODateWithMs)}};
}

PeerIdentity identityFromJson(const QJsonObject& object) {
    return {object.value("id").toString(), object.value("name").toString(),
            object.value("device").toString(),
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

qint64 MeshCoordinator::revision() const {
    return revision_;
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
    revision_ = 1;
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
    revision_ = 2;
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
    const bool added = peers_.remember(peer);
    if (added) {
        ++revision_;
    }
    return added;
}

bool MeshCoordinator::forgetPeer(const QString& peerId) {
    if (!peers_.remove(peerId)) {
        return false;
    }
    retryCounts_.remove(peerId);
    retryScheduled_.remove(peerId);
    degradedPeers_.remove(peerId);
    ++retryGenerations_[peerId];
    linkGenerations_.remove(peerId);
    ++revision_;
    updateState();
    return true;
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
    if (changed) {
        ++revision_;
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

bool MeshCoordinator::canAttemptLink(const QString& peerId) const {
    return !retryScheduled_.contains(peerId);
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
    if (peer.peerId.isEmpty() || meshId_.isEmpty() || retryScheduled_.contains(peer.peerId)) {
        return;
    }
    const int retryIndex = retryCounts_.value(peer.peerId);
    int delay{};
    if (retryIndex >= static_cast<int>(policy_.meshRetryDelaysSeconds.size())) {
        degradedPeers_.insert(peer.peerId);
        updateState();
        delay = QRandomGenerator::global()->bounded(120, 301);
    } else {
        delay = policy_.meshRetryDelaysSeconds[retryIndex];
    }

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

quint64 MeshCoordinator::nextLinkGeneration(const QString& peerId) {
    return ++linkGenerations_[peerId];
}

bool MeshCoordinator::acceptLinkGeneration(const QString& peerId, quint64 generation) {
    if (peerId.isEmpty() || generation == 0 || generation < linkGenerations_.value(peerId)) {
        return false;
    }
    linkGenerations_.insert(peerId, generation);
    return true;
}

void MeshCoordinator::resetRetryBackoff() {
    for (auto iterator = retryGenerations_.begin(); iterator != retryGenerations_.end();
         ++iterator) {
        ++iterator.value();
    }
    retryCounts_.clear();
    retryScheduled_.clear();
    degradedPeers_.clear();
    updateState();
}

void MeshCoordinator::routeAvailable(const QString& peerId) {
    if (peerId.isEmpty() || !degradedPeers_.contains(peerId)) {
        return;
    }
    retryCounts_.remove(peerId);
    retryScheduled_.remove(peerId);
    degradedPeers_.remove(peerId);
    ++retryGenerations_[peerId];
    updateState();
}

void MeshCoordinator::resetRuntime() {
    peers_.clear();
    retryCounts_.clear();
    retryGenerations_.clear();
    linkGenerations_.clear();
    retryScheduled_.clear();
    degradedPeers_.clear();
    revision_ = 0;
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
