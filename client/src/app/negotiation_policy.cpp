#include "tmc/app/negotiation_policy.h"

namespace tmc {

bool shouldInitiateNegotiation(const QString& localPeerId, const QString& remotePeerId) {
    return !localPeerId.isEmpty() && !remotePeerId.isEmpty() &&
           localPeerId.compare(remotePeerId, Qt::CaseSensitive) < 0;
}

} // namespace tmc
