#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>

namespace tmc {

struct ChatMessage {
    QString messageId;
    QString senderId;
    QString text;
    qint64 logicalClock{0};
    QDateTime createdAt;

    bool isValid() const;
};

} // namespace tmc

Q_DECLARE_METATYPE(tmc::ChatMessage)
