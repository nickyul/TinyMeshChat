#pragma once

#include <array>

namespace tmc {

struct ConnectionPolicy {
    int iceGatheringTimeoutSeconds{60};
    int connectionTimeoutSeconds{60};
    int manualSignalingTimeoutSeconds{20 * 60};
    int heartbeatIntervalSeconds{10};
    int livenessTimeoutSeconds{35};
    int maxPeers{6};
    std::array<int, 3> meshRetryDelaysSeconds{5, 15, 30};

    constexpr bool isValid() const {
        return iceGatheringTimeoutSeconds > 0 && connectionTimeoutSeconds > 0 &&
               manualSignalingTimeoutSeconds > 0 && heartbeatIntervalSeconds > 0 &&
               livenessTimeoutSeconds > heartbeatIntervalSeconds && maxPeers >= 2 &&
               maxPeers <= 16 && meshRetryDelaysSeconds[0] > 0 &&
               meshRetryDelaysSeconds[1] >= meshRetryDelaysSeconds[0] &&
               meshRetryDelaysSeconds[2] >= meshRetryDelaysSeconds[1];
    }
};

} // namespace tmc
