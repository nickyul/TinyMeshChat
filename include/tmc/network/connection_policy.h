#pragma once

#include <array>

namespace tmc {

struct ConnectionPolicy {
    int iceGatheringTimeoutSeconds{60};
    int connectionTimeoutSeconds{60};
    int helloTimeoutSeconds{10};
    int manualSignalingTimeoutSeconds{20 * 60};
    int heartbeatIntervalSeconds{10};
    int livenessTimeoutSeconds{35};
    int maxPeers{6};
    std::array<int, 5> meshRetryDelaysSeconds{1, 3, 10, 30, 60};
    int degradedRetryMinSeconds{120};
    int degradedRetryMaxSeconds{300};

    constexpr bool isValid() const {
        return iceGatheringTimeoutSeconds > 0 && connectionTimeoutSeconds > 0 &&
               helloTimeoutSeconds > 0 &&
               manualSignalingTimeoutSeconds > 0 && heartbeatIntervalSeconds > 0 &&
               livenessTimeoutSeconds > heartbeatIntervalSeconds && maxPeers >= 2 &&
               maxPeers <= 16 && meshRetryDelaysSeconds[0] > 0 &&
               meshRetryDelaysSeconds[1] >= meshRetryDelaysSeconds[0] &&
               meshRetryDelaysSeconds[2] >= meshRetryDelaysSeconds[1] &&
               meshRetryDelaysSeconds[3] >= meshRetryDelaysSeconds[2] &&
               meshRetryDelaysSeconds[4] >= meshRetryDelaysSeconds[3] &&
               degradedRetryMinSeconds >= meshRetryDelaysSeconds[4] &&
               degradedRetryMaxSeconds >= degradedRetryMinSeconds;
    }
};

} // namespace tmc
