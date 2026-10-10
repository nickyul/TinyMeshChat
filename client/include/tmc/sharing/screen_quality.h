#pragma once

#include <QJsonObject>
#include <QSize>

namespace tmc {

struct ScreenQuality {
    int height{1080};
    int fps{30};
    bool isValid() const;
    int maxBitrate() const;
    QSize fittedSize(QSize source) const;
    ScreenQuality boundedBy(ScreenQuality ceiling) const;
    bool operator==(const ScreenQuality&) const = default;
};

} // namespace tmc
