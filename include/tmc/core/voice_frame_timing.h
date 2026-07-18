#pragma once

#include <QtGlobal>

namespace tmc {

struct VoiceFrameTiming {
    qint64 encodedAtNs{0};
    double captureQueueMs{0.0};
    double dspMs{0.0};
    double encodeMs{0.0};
};

} // namespace tmc
