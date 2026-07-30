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
    int expectedDeliveries{0};
};

struct IncomingChatMessage {
    std::optional<ChatMessage> message;
    Packet acknowledgement;
};

class MessagingService {
public:
    void clear();

    Result<OutgoingChatMessage> createMessage(const QString& text, const QString& meshId,
                                              const QString& senderId,
                                              const QSet<QString>& targets);
    Result<IncomingChatMessage> receiveMessage(const Packet& packet,
                                               const Packet& acknowledgement);
    bool receiveAcknowledgement(const Packet& packet);

    QPair<int, int> deliveryCounts(const QString& messageId) const;

private:
    bool rememberMessage(const QString& messageId);

    DeliveryTracker delivery_;
    qint64 logicalClock_{0};
    QSet<QString> seenMessageIds_;
    QQueue<QString> seenMessageOrder_;
};

} // namespace tmc
