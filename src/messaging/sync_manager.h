#pragma once
#include "messaging/chat_message.h"
#include <QList>
#include <QSet>
namespace tmc {
class SyncManager {
  public:
    static QSet<QString> recentIds(const QList<ChatMessage>&, qsizetype limit = 500);
    static QList<ChatMessage> missingFrom(const QList<ChatMessage>&, const QSet<QString>& remoteIds,
                                          qsizetype limit = 500);
};
} // namespace tmc
