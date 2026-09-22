#pragma once

#include "tmc/signaling_protocol/envelope_codec.h"
#include "tmc/signaling_protocol/room_creation.h"

namespace tmc::signaling_protocol {

// Typed messages <-> validated envelopes. EnvelopeCodec handles the JSON bytes.
class RoomCreationCodec {
public:
    using EnvelopeResult = std::variant<Envelope, CodecError>;
    using RequestResult = std::variant<CreateRoomRequest, CodecError>;
    using ResponseResult = std::variant<RoomCreatedResponse, CodecError>;

    [[nodiscard]] static EnvelopeResult toEnvelope(const CreateRoomRequest& request);
    [[nodiscard]] static EnvelopeResult toEnvelope(const RoomCreatedResponse& response);
    [[nodiscard]] static RequestResult decodeRequest(const Envelope& envelope);
    [[nodiscard]] static ResponseResult decodeResponse(const Envelope& envelope);
};

} // namespace tmc::signaling_protocol
