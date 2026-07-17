#include "tmc/signaling/invitation.h"

namespace tmc {

bool Invitation::isExpired(const QDateTime& now) const {
    return expiresAt < now;
}

} // namespace tmc
