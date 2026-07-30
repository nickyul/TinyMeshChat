#include "tmc/messaging/chat_message.h"

#include "tmc/core/limits.h"
#include "tmc/core/uuid.h"

namespace tmc {

bool ChatMessage::isValid() const {
    return isCanonicalUuid(messageId) && isCanonicalUuid(senderId) && logicalClock > 0 &&
           !text.trimmed().isEmpty() && text.size() <= limits::MaxChatMessageLength &&
           createdAt.isValid();
}

} // namespace tmc
