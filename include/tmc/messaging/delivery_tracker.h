#pragma once

#include <QHash>
#include <QQueue>
#include <QSet>
#include <QString>

namespace tmc {

class DeliveryTracker {
public:
    void clear();
    void track(const QString& messageId, const QSet<QString>& expectedPeers);
    bool acknowledge(const QString& messageId, const QString& peerId);

    int deliveredCount(const QString& messageId) const;
    int expectedCount(const QString& messageId) const;

private:
    struct DeliveryState {
        QSet<QString> expectedPeers;
        QSet<QString> acknowledgedPeers;
    };

    QHash<QString, DeliveryState> states_;
    QQueue<QString> insertionOrder_;
};

} // namespace tmc
