#include "tmc/app/topology_controller.h"

namespace tmc {

bool TopologyController::shouldInitiateLink(const QString& localPeerId,
                                            const QString& remotePeerId) const {
    return !localPeerId.isEmpty() && !remotePeerId.isEmpty() &&
           localPeerId.compare(remotePeerId, Qt::CaseSensitive) < 0;
}

} // namespace tmc
