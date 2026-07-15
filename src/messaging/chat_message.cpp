#include "messaging/chat_message.h"
#include <QUuid>
using namespace tmc;
bool ChatMessage::isValid() const {
    return !QUuid::fromString(messageId).isNull() && !QUuid::fromString(roomId).isNull() &&
           !senderId.isEmpty() && logicalClock > 0 && text.size() <= 4096 && createdAt.isValid() &&
           receivedAt.isValid();
}
