#include "tmc/sharing/screen_quality.h"

#include <algorithm>

namespace tmc {

bool ScreenQuality::isValid() const {
    return (height == 720 || height == 1080 || height == 1440) && (fps == 30 || fps == 60);
}

int ScreenQuality::maxBitrate() const {
    if (height == 720) {
        return fps == 60 ? 4000000 : 2000000;
    }
    if (height == 1440) {
        return fps == 60 ? 12000000 : 8000000;
    }
    return fps == 60 ? 8000000 : 4000000;
}

QSize ScreenQuality::fittedSize(QSize source) const {
    if (!source.isValid() || source.isEmpty()) {
        return {};
    }
    QSize box(height * 16 / 9, height);
    if (source.height() > source.width()) {
        box.transpose();
    }
    if (source.width() > box.width() || source.height() > box.height()) {
        source.scale(box, Qt::KeepAspectRatio);
    }
    return {std::max(2, source.width() & ~1), std::max(2, source.height() & ~1)};
}

ScreenQuality ScreenQuality::boundedBy(ScreenQuality ceiling) const {
    return {std::min(height, ceiling.height), std::min(fps, ceiling.fps)};
}

} // namespace tmc
