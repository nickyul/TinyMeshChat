#pragma once

#include <QString>

namespace tmc {

bool shouldInitiateNegotiation(const QString& localPeerId, const QString& remotePeerId);

} // namespace tmc
