#pragma once

#include "tmc/signaling_protocol/envelope_codec.h"

namespace tmc::signaling_protocol {

inline constexpr qsizetype RoomCapacity = 6;
inline constexpr qsizetype MaxSignalBytes = 32 * 1024;
inline constexpr int InvitationLifetimeSeconds = 10 * 60;
inline constexpr qsizetype MaxAcquaintances = 256;
inline constexpr int OnlineInvitationLifetimeSeconds = 60;

// Schema validation only. Membership, token lifetime and replay are server policy.
class MessageCodec {
public:
    [[nodiscard]] static std::optional<CodecError> validateRequest(const Envelope& envelope);
    [[nodiscard]] static std::optional<CodecError> validateServerMessage(const Envelope& envelope);
    [[nodiscard]] static Envelope error(const std::optional<QString>& requestId, const QString& code);
};

} // namespace tmc::signaling_protocol
