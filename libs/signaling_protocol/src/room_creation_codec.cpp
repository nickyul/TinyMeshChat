#include "tmc/signaling_protocol/room_creation_codec.h"

#include <QUuid>

namespace tmc::signaling_protocol {
namespace {

std::optional<CodecError> validateHeader(const Envelope& envelope, const QString& type) {
    if (const auto failure = EnvelopeCodec::validate(envelope)) {
        return failure;
    }
    if (envelope.type != type) {
        return CodecError{CodecErrorCode::InvalidType, QStringLiteral("Unexpected message type")};
    }
    if (!envelope.requestId) {
        return CodecError{CodecErrorCode::InvalidRequestId,
                          QStringLiteral("Request identifier is required")};
    }
    return std::nullopt;
}

bool isCanonicalUuid(const QString& text) {
    const auto uuid = QUuid::fromString(text);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == text;
}

} // namespace

RoomCreationCodec::EnvelopeResult RoomCreationCodec::toEnvelope(const CreateRoomRequest& request) {
    Envelope envelope{1, QStringLiteral("room.create"), request.requestId, {}};
    const auto decoded = decodeRequest(envelope);
    if (const auto* failure = std::get_if<CodecError>(&decoded)) {
        return *failure;
    }
    return envelope;
}

RoomCreationCodec::EnvelopeResult RoomCreationCodec::toEnvelope(const RoomCreatedResponse& response) {
    Envelope envelope{1, QStringLiteral("room.created"), response.requestId,
                      {{QStringLiteral("roomId"), response.roomId},
                       {QStringLiteral("peerId"), response.peerId}}};
    const auto decoded = decodeResponse(envelope);
    if (const auto* failure = std::get_if<CodecError>(&decoded)) {
        return *failure;
    }
    return envelope;
}

RoomCreationCodec::RequestResult RoomCreationCodec::decodeRequest(const Envelope& envelope) {
    if (const auto failure = validateHeader(envelope, QStringLiteral("room.create"))) {
        return *failure;
    }
    if (!envelope.body.isEmpty()) {
        return CodecError{CodecErrorCode::UnknownField,
                          QStringLiteral("Room creation request body must be empty")};
    }
    return CreateRoomRequest{*envelope.requestId};
}

RoomCreationCodec::ResponseResult RoomCreationCodec::decodeResponse(const Envelope& envelope) {
    if (const auto failure = validateHeader(envelope, QStringLiteral("room.created"))) {
        return *failure;
    }
    for (auto it = envelope.body.constBegin(); it != envelope.body.constEnd(); ++it) {
        if (it.key() != "roomId" && it.key() != "peerId") {
            return CodecError{CodecErrorCode::UnknownField,
                              QStringLiteral("Unknown room creation response field")};
        }
    }
    const auto roomId = envelope.body.value(QStringLiteral("roomId"));
    const auto peerId = envelope.body.value(QStringLiteral("peerId"));
    if (!roomId.isString() || !peerId.isString() ||
        !isCanonicalUuid(roomId.toString()) || !isCanonicalUuid(peerId.toString())) {
        return CodecError{CodecErrorCode::InvalidBody,
                          QStringLiteral("Room and peer identifiers must be canonical non-null UUIDs")};
    }
    return RoomCreatedResponse{*envelope.requestId, roomId.toString(), peerId.toString()};
}

} // namespace tmc::signaling_protocol
