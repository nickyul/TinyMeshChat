#include "tmc/app/messaging_service.h"

#include "tmc/core/limits.h"
#include "tmc/core/uuid.h"

#include <QDateTime>

#include <limits>

namespace tmc {

namespace {

constexpr qsizetype MaxSeenMessages = 4096;
constexpr qint64 MaxLogicalClock = std::numeric_limits<qint64>::max();

} // namespace

void MessagingService::clear() {
    delivery_.clear();
    logicalClock_ = 0;
    seenMessageIds_.clear();
    seenMessageOrder_.clear();
}

Result<OutgoingChatMessage> MessagingService::createMessage(const QString& text,
                                                            const QString& meshId,
                                                            const QString& senderId,
                                                            const QSet<QString>& expectedPeers) {
    const auto normalized = text.trimmed();
    if (normalized.isEmpty()) {
        return Result<OutgoingChatMessage>::failure("Сообщение пустое.");
    }
    if (normalized.size() > limits::MaxChatMessageLength) {
        return Result<OutgoingChatMessage>::failure(
            "Максимальная длина сообщения — 4096 символов.");
    }
    if (!isCanonicalUuid(meshId) || !isCanonicalUuid(senderId)) {
        return Result<OutgoingChatMessage>::failure(
            "Некорректные идентификаторы сообщения.");
    }
    if (logicalClock_ == MaxLogicalClock) {
        return Result<OutgoingChatMessage>::failure(
            "Исчерпан диапазон логических часов.");
    }

    const auto now = QDateTime::currentDateTimeUtc();
    ChatMessage message{createUuid(), senderId, normalized, ++logicalClock_, now};
    if (!message.isValid()) {
        return Result<OutgoingChatMessage>::failure("Не удалось создать сообщение.");
    }
    rememberMessage(message.messageId);
    delivery_.track(message.messageId, expectedPeers);
    Packet packet{PacketType::ChatMessage,
                  createUuid(),
                  meshId,
                  senderId,
                  now,
                  ChatMessagePayload{message.messageId, message.text, message.logicalClock}};
    return Result<OutgoingChatMessage>::success({message, packet});
}

Result<std::optional<ChatMessage>> MessagingService::receiveMessage(const Packet& packet) {
    const auto& payload = std::get<ChatMessagePayload>(packet.payload);
    const auto remoteClock = payload.logicalClock;
    ChatMessage message{payload.messageId, packet.senderId, payload.text, remoteClock,
                        packet.createdAt};
    if (!message.isValid()) {
        return Result<std::optional<ChatMessage>>::failure(
            "Получено некорректное сообщение.");
    }
    if (seenMessageIds_.contains(message.messageId)) {
        return Result<std::optional<ChatMessage>>::success(std::nullopt);
    }
    const auto currentClock = qMax(logicalClock_, remoteClock);
    if (currentClock == MaxLogicalClock) {
        return Result<std::optional<ChatMessage>>::failure(
            "Исчерпан диапазон логических часов.");
    }
    logicalClock_ = currentClock + 1;
    rememberMessage(message.messageId);
    return Result<std::optional<ChatMessage>>::success(std::move(message));
}

bool MessagingService::receiveAcknowledgement(const QString& messageId,
                                              const QString& peerId) {
    return delivery_.acknowledge(messageId, peerId);
}

QPair<int, int> MessagingService::deliveryCounts(const QString& messageId) const {
    return {delivery_.deliveredCount(messageId), delivery_.expectedCount(messageId)};
}

bool MessagingService::rememberMessage(const QString& messageId) {
    if (seenMessageIds_.contains(messageId)) {
        return false;
    }
    if (seenMessageOrder_.size() >= MaxSeenMessages) {
        seenMessageIds_.remove(seenMessageOrder_.dequeue());
    }
    seenMessageIds_.insert(messageId);
    seenMessageOrder_.enqueue(messageId);
    return true;
}

} // namespace tmc
