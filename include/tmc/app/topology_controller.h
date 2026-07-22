#pragma once

#include <QString>

namespace tmc {

class TopologyController final {
public:
    bool shouldInitiateLink(const QString& localPeerId, const QString& remotePeerId) const;
};

} // namespace tmc
