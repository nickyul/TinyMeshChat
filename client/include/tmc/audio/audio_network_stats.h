#pragma once

#include <QtGlobal>

namespace tmc {

struct AudioNetworkStats {
    double packetLossPercent{0.0};
    int jitterMs{0};
    int bufferMs{0};
    quint64 receivedPackets{0};
    quint64 concealedFrames{0};
    quint64 fecRecoveredFrames{0};
    quint64 latePackets{0};
};

} // namespace tmc
