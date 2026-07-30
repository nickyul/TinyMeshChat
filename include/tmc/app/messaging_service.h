#pragma once

#include "tmc/core/result.h"
#include "tmc/messaging/chat_message.h"
#include "tmc/messaging/delivery_tracker.h"
#include "tmc/protocol/packet.h"

#include <QQueue>
#include <QSet>

#include <optional>

namespace tmc {

struct OutgoingChatMessage {
    ChatMessage message;
    Packet packet;
};

class MessagingService {
public:
    void clear();

    Result<OutgoingChatMessage> createMessage(const QString& text, const QString& meshId,
                                              const QString& senderId,
                                              const QSet<QString>& expectedPeers);
    Result<std::optional<ChatMessage>> receiveMessage(const Packet& packet);
    bool receiveAcknowledgement(const QString& messageId, const QString& peerId);

    QPair<int, int> deliveryCounts(const QString& messageId) const;

private:
    bool rememberMessage(const QString& messageId);

    DeliveryTracker delivery_;
    qint64 logicalClock_{0};
    QSet<QString> seenMessageIds_;
    QQueue<QString> seenMessageOrder_;
};

} // namespace tmc
