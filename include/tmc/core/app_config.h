#pragma once

#include "tmc/core/result.h"

#include <QStringList>

namespace tmc {

struct AudioPreferences {
    QString captureDevice;
    QString playbackDevice;
    bool echoCancellation{false};
    bool noiseSuppression{true};
    bool automaticGainControl{true};
    int outputVolume{100};
    int qualityKbps{48};

    bool isValid() const;
};

struct AppConfig {
    QStringList stunServers{"stun:stun.l.google.com:19302", "stun:stun.cloudflare.com:3478"};
    AudioPreferences audio;

    static Result<AppConfig> load(const QString& path);
    Result<void> save(const QString& path) const;
};

} // namespace tmc
