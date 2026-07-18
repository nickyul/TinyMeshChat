#include "tmc/app/messaging_service.h"

#include "tmc/protocol/packet_codec.h"

#include <QDateTime>
#include <QUuid>

namespace tmc {

namespace {

QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

void MessagingService::clear() {
    delivery_ = {};
    logicalClock_ = 0;
    seenMessageIds_.clear();
    seenMessageOrder_.clear();
}

Result<OutgoingChatMessage> MessagingService::createMessage(const QString& text,
                                                            const QString& meshId,
                                                            const QString& senderId,
                                                            const QSet<QString>& targets) {
    const auto normalized = text.trimmed();
    if (normalized.isEmpty()) {
        return Result<OutgoingChatMessage>::failure("Сообщение пустое.");
    }
    if (normalized.size() > PacketCodec::MaxTextChars) {
        return Result<OutgoingChatMessage>::failure(
            "Максимальная длина сообщения — 4096 символов.");
    }

    const auto now = QDateTime::currentDateTimeUtc();
    ChatMessage message{uuid(), meshId, senderId, normalized, ++logicalClock_, now, now};
    rememberMessage(message.messageId);
    delivery_.track(message.messageId, targets);
    Packet packet{PacketType::ChatMessage,
                  uuid(),
                  meshId,
                  senderId,
                  now,
                  ChatMessagePayload{message.messageId, message.text, message.logicalClock}};
    return Result<OutgoingChatMessage>::success(
        {message, packet, static_cast<int>(targets.size())});
}

Result<IncomingChatMessage> MessagingService::receiveMessage(const Packet& packet,
                                                             const QString& meshId,
                                                             const Packet& acknowledgement) {
    const auto& payload = std::get<ChatMessagePayload>(packet.payload);
    const auto remoteClock = payload.logicalClock;
    logicalClock_ = qMax(logicalClock_, remoteClock) + 1;
    const auto now = QDateTime::currentDateTimeUtc();
    ChatMessage message{payload.messageId, meshId, packet.senderId, payload.text, remoteClock,
                        packet.createdAt,  now};
    if (!message.isValid()) {
        return Result<IncomingChatMessage>::failure("Получено некорректное сообщение.");
    }
    IncomingChatMessage result;
    if (rememberMessage(message.messageId)) {
        result.message = message;
    }
    result.acknowledgement = acknowledgement;
    return Result<IncomingChatMessage>::success(result);
}

bool MessagingService::receiveAcknowledgement(const Packet& packet) {
    return delivery_.acknowledge(std::get<ChatAckPayload>(packet.payload).messageId,
                                 packet.senderId);
}

QPair<int, int> MessagingService::deliveryCounts(const QString& messageId) const {
    return {delivery_.deliveredCount(messageId), delivery_.expectedCount(messageId)};
}

bool MessagingService::rememberMessage(const QString& messageId) {
    if (seenMessageIds_.contains(messageId)) {
        return false;
    }
    constexpr qsizetype MaxSeenMessages = 4096;
    if (seenMessageOrder_.size() >= MaxSeenMessages) {
        seenMessageIds_.remove(seenMessageOrder_.dequeue());
    }
    seenMessageIds_.insert(messageId);
    seenMessageOrder_.enqueue(messageId);
    return true;
}

} // namespace tmc
