#include "tmc/identity/peer_identity.h"

#include <QUuid>

namespace tmc {

namespace {

bool isCanonicalUuid(const QString& value) {
    const auto uuid = QUuid::fromString(value);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value;
}

} // namespace

bool PeerIdentity::isValid() const {
    return isCanonicalUuid(peerId) && !displayName.trimmed().isEmpty();
}

} // namespace tmc
