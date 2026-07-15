#include "messaging/sync_manager.h"
using namespace tmc;
QSet<QString> SyncManager::recentIds(const QList<ChatMessage>& m, qsizetype limit) {
    QSet<QString> s;
    for (qsizetype i = qMax<qsizetype>(0, m.size() - limit); i < m.size(); ++i)
        s.insert(m[i].messageId);
    return s;
}
QList<ChatMessage> SyncManager::missingFrom(const QList<ChatMessage>& m, const QSet<QString>& ids,
                                            qsizetype limit) {
    QList<ChatMessage> r;
    for (auto it = m.crbegin(); it != m.crend() && r.size() < limit; ++it)
        if (!ids.contains(it->messageId))
            r.prepend(*it);
    return r;
}
