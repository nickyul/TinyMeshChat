#pragma once

#include "tmc/core/result.h"

#include <QJsonObject>

namespace tmc {

inline constexpr int ScreenProtocolVersion = 2;
Result<QJsonObject> decodeScreenControl(const QJsonObject& message);

} // namespace tmc
