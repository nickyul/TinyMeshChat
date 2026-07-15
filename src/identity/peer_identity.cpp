#include "identity/peer_identity.h"
#include <QUuid>
using namespace tmc;
static bool uuid(const QString& s) {
    return !QUuid::fromString(s).isNull();
}
bool PeerIdentity::isValid() const {
    return uuid(peerId) && uuid(deviceId) && !displayName.trimmed().isEmpty() &&
           createdAt.isValid();
}
