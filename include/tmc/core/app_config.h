#pragma once

#include "tmc/core/result.h"

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace tmc {

enum class AudioInputMode { VoiceActivity = 0, PushToTalk = 1 };
enum class NoiseSuppressionLevel { Low = 0, Moderate = 1, High = 2, VeryHigh = 3 };
enum class PttBindingType { Keyboard = 0, Mouse = 1 };

struct PttBinding {
    PttBindingType type{PttBindingType::Keyboard};
    QString platform;
    quint32 code{'V'};
    QString displayName{"V"};

    bool isValid() const;
};

struct AudioPreferences {
    QString captureDevice;
    QString playbackDevice;
    bool echoCancellation{false};
    bool highPassFilter{true};
    bool noiseSuppression{true};
    NoiseSuppressionLevel noiseSuppressionLevel{NoiseSuppressionLevel::Moderate};
    bool automaticGainControl{true};
    int agcTargetLevelDbfs{3};
    int agcCompressionGainDb{9};
    bool agcLimiter{true};
    int outputVolume{100};
    int qualityKbps{48};

    AudioInputMode inputMode{AudioInputMode::VoiceActivity};
    double vadThreshold{0.15};
    int vadHangoverMs{200};
    PttBinding pttBinding;

    bool isValid() const;
};

struct AppConfig {
    QStringList stunServers;
    AudioPreferences audio;

    static Result<AppConfig> load(const QString& path);
    Result<void> save(const QString& path) const;
};

} // namespace tmc
