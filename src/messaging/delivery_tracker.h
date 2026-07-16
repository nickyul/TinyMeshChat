#pragma once
#include <QHash>
#include <QSet>
#include <QString>
namespace tmc {
class DeliveryTracker {
  public:
    void track(const QString&, const QSet<QString>&);
    bool acknowledge(const QString&, const QString&);
    int deliveredCount(const QString&) const;
    int expectedCount(const QString&) const;
    bool fullyDelivered(const QString&) const;

  private:
    QHash<QString, QSet<QString>> expected_, acks_;
};
} // namespace tmc
