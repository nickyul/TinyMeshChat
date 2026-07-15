#pragma once
#include <QDateTime>
#include <QString>
namespace tmc {
struct ChatMessage {
    QString messageId, roomId, senderId, text;
    qint64 logicalClock{0};
    QDateTime createdAt, receivedAt;
    bool isValid() const;
};
} // namespace tmc
