#pragma once
#include "core/result.h"
#include "messaging/chat_message.h"
#include <QSqlDatabase>
namespace tmc {
class MessageRepository {
  public:
    explicit MessageRepository(QSqlDatabase db) : db_(std::move(db)) {}
    Result<bool> insert(const ChatMessage&);
    Result<QList<ChatMessage>> history(const QString&, int limit = 500) const;
    Result<QList<ChatMessage>> recent(const QString&, int limit = 500) const;
    Result<QList<ChatMessage>> byIds(const QString&, const QStringList&) const;
    Result<QList<ChatMessage>> pendingForPeer(const QString&, const QString&,
                                              int limit = 500) const;
    Result<qint64> count(const QString&) const;
    Result<QPair<int, int>> deliveryCounts(const QString&) const;
    Result<void> setDelivery(const QString&, const QString&, const QString&);

  private:
    QSqlDatabase db_;
};
} // namespace tmc
