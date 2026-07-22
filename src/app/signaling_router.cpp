#include "tmc/app/signaling_router.h"

#include <QDateTime>

namespace tmc {

namespace {

constexpr qint64 RouteLifetimeMs = 2 * 60 * 1000;
constexpr int MaxRememberedPackets = 2048;

} // namespace

void SignalingRouter::clear() {
    routes_.clear();
    reverseRoutes_.clear();
    seenPackets_.clear();
}

bool SignalingRouter::rememberPacket(const QString& packetId) {
    expire();
    if (seenPackets_.contains(packetId)) {
        return false;
    }
    if (seenPackets_.size() >= MaxRememberedPackets) {
        seenPackets_.clear();
    }
    seenPackets_.insert(packetId, QDateTime::currentMSecsSinceEpoch());
    return true;
}

void SignalingRouter::observeDirect(const QString& peerId, const QString& connectionId) {
    observeRoute(peerId, connectionId, 1);
}

void SignalingRouter::observeRoute(const QString& peerId, const QString& connectionId, int hops) {
    if (peerId.isEmpty() || connectionId.isEmpty() || hops <= 0) {
        return;
    }
    expire();
    const auto current = routes_.constFind(peerId);
    if (current == routes_.cend() || hops <= current->hops ||
        current->connectionId == connectionId) {
        routes_.insert(peerId,
                       Route{connectionId, hops, QDateTime::currentMSecsSinceEpoch()});
    }
}

void SignalingRouter::forgetConnection(const QString& connectionId) {
    for (auto iterator = routes_.begin(); iterator != routes_.end();) {
        if (iterator->connectionId == connectionId) {
            iterator = routes_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = reverseRoutes_.begin(); iterator != reverseRoutes_.end();) {
        if (iterator.value() == connectionId) {
            iterator = reverseRoutes_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void SignalingRouter::forgetPeer(const QString& peerId) {
    routes_.remove(peerId);
}

std::optional<QString> SignalingRouter::nextHop(const QString& peerId,
                                                const QString& excludedConnection) const {
    const auto route = routes_.constFind(peerId);
    if (route == routes_.cend() || route->connectionId == excludedConnection ||
        QDateTime::currentMSecsSinceEpoch() - route->updatedAtMs > RouteLifetimeMs) {
        return std::nullopt;
    }
    return route->connectionId;
}

void SignalingRouter::rememberReverseRoute(const QString& requestId,
                                           const QString& connectionId) {
    if (!requestId.isEmpty() && !connectionId.isEmpty()) {
        reverseRoutes_.insert(requestId, connectionId);
    }
}

std::optional<QString> SignalingRouter::reverseHop(const QString& requestId) const {
    const auto iterator = reverseRoutes_.constFind(requestId);
    if (iterator == reverseRoutes_.cend()) {
        return std::nullopt;
    }
    return iterator.value();
}

void SignalingRouter::expire() {
    const auto threshold = QDateTime::currentMSecsSinceEpoch() - RouteLifetimeMs;
    for (auto iterator = routes_.begin(); iterator != routes_.end();) {
        if (iterator->updatedAtMs < threshold) {
            iterator = routes_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = seenPackets_.begin(); iterator != seenPackets_.end();) {
        if (iterator.value() < threshold) {
            iterator = seenPackets_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

} // namespace tmc
