#pragma once

#include <QtGlobal>

namespace tmc::server {

inline constexpr qsizetype MaxSessions = 128;
// Server policy; currently matches the client's six-member mesh limit.
inline constexpr qsizetype RoomCapacity = 6;

} // namespace tmc::server
