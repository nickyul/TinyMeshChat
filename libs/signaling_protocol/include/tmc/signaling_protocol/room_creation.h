#pragma once

#include <QString>

namespace tmc::signaling_protocol {

struct CreateRoomRequest {
    QString requestId;
};

struct RoomCreatedResponse {
    QString requestId;
    QString roomId;
    // Identifies this room membership, not the client's persistent identity.
    QString peerId;
};

} // namespace tmc::signaling_protocol
