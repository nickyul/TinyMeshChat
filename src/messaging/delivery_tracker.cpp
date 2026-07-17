#include "tmc/messaging/delivery_tracker.h"

namespace tmc {

void DeliveryTracker::track(const QString& m, const QSet<QString>& p) {
    if (!expected_.contains(m) && expected_.size() >= 4096) {
        const auto oldest = expected_.constBegin().key();
        expected_.remove(oldest);
        acks_.remove(oldest);
    }
    expected_[m].unite(p);
    acks_[m];
}

bool DeliveryTracker::acknowledge(const QString& m, const QString& p) {
    if (!expected_.contains(m) || !expected_[m].contains(p)) {
        return false;
    }
    auto n = acks_[m].size();
    acks_[m].insert(p);
    return acks_[m].size() != n;
}

int DeliveryTracker::deliveredCount(const QString& m) const {
    return acks_.value(m).size();
}

int DeliveryTracker::expectedCount(const QString& m) const {
    return expected_.value(m).size();
}

bool DeliveryTracker::fullyDelivered(const QString& m) const {
    return expected_.contains(m) && acks_.value(m) == expected_.value(m);
}

} // namespace tmc
