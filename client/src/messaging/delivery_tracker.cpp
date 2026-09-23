#include "tmc/messaging/delivery_tracker.h"

namespace tmc {

namespace {

constexpr qsizetype MaxTrackedMessages = 4096;

} // namespace

void DeliveryTracker::clear() {
    states_.clear();
    insertionOrder_.clear();
}

void DeliveryTracker::track(const QString& messageId,
                            const QSet<QString>& expectedPeers) {
    const auto existing = states_.find(messageId);
    if (existing != states_.end()) {
        existing->expectedPeers.unite(expectedPeers);
        return;
    }

    if (states_.size() >= MaxTrackedMessages) {
        states_.remove(insertionOrder_.dequeue());
    }
    states_.insert(messageId, DeliveryState{expectedPeers, {}});
    insertionOrder_.enqueue(messageId);
}

bool DeliveryTracker::acknowledge(const QString& messageId, const QString& peerId) {
    const auto state = states_.find(messageId);
    if (state == states_.end() || !state->expectedPeers.contains(peerId) ||
        state->acknowledgedPeers.contains(peerId)) {
        return false;
    }
    state->acknowledgedPeers.insert(peerId);
    return true;
}

int DeliveryTracker::deliveredCount(const QString& messageId) const {
    const auto state = states_.constFind(messageId);
    return state == states_.cend() ? 0 : state->acknowledgedPeers.size();
}

int DeliveryTracker::expectedCount(const QString& messageId) const {
    const auto state = states_.constFind(messageId);
    return state == states_.cend() ? 0 : state->expectedPeers.size();
}

} // namespace tmc
