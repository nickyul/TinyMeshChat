#include "tmc/core/app_config.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace tmc {

namespace {

constexpr qsizetype MaxStunUriLength = 512;
constexpr qsizetype MaxPttBindingNameLength = 64;
constexpr quint32 MaxSerializedPttCode = 65535;
constexpr quint32 MaxWindowsVirtualKey = 0xFE;
constexpr quint32 MaxMacVirtualKey = 0x7F;
constexpr quint32 WindowsMiddleMouseButton = 0x04;
constexpr quint32 WindowsBackMouseButton = 0x05;
constexpr quint32 WindowsForwardMouseButton = 0x06;
constexpr quint32 MacMiddleMouseButton = 2;
constexpr quint32 MacBackMouseButton = 3;
constexpr quint32 MacForwardMouseButton = 4;

bool isIntegerInRange(double value, double minimum, double maximum) {
    return std::isfinite(value) && value == std::floor(value) && value >= minimum &&
           value <= maximum;
}

bool isWindowsMouseVirtualKey(quint32 code) {
    return code == 0x01 || code == 0x02 || code == WindowsMiddleMouseButton ||
           code == WindowsBackMouseButton || code == WindowsForwardMouseButton;
}

QString inputModeName(AudioInputMode mode) {
    return mode == AudioInputMode::PushToTalk ? "push_to_talk" : "voice_activity";
}

std::optional<AudioInputMode> inputModeFromName(const QString& name) {
    if (name == "voice_activity") {
        return AudioInputMode::VoiceActivity;
    }
    if (name == "push_to_talk") {
        return AudioInputMode::PushToTalk;
    }
    return std::nullopt;
}

QString noiseSuppressionLevelName(NoiseSuppressionLevel level) {
    switch (level) {
    case NoiseSuppressionLevel::Low:
        return "low";
    case NoiseSuppressionLevel::Moderate:
        return "moderate";
    case NoiseSuppressionLevel::High:
        return "high";
    case NoiseSuppressionLevel::VeryHigh:
        return "very_high";
    }
    return "moderate";
}

std::optional<NoiseSuppressionLevel> noiseSuppressionLevelFromName(const QString& name) {
    if (name == "low") {
        return NoiseSuppressionLevel::Low;
    }
    if (name == "moderate") {
        return NoiseSuppressionLevel::Moderate;
    }
    if (name == "high") {
        return NoiseSuppressionLevel::High;
    }
    if (name == "very_high") {
        return NoiseSuppressionLevel::VeryHigh;
    }
    return std::nullopt;
}

QString pttBindingTypeName(PttBindingType type) {
    return type == PttBindingType::Mouse ? "mouse" : "keyboard";
}

std::optional<PttBindingType> pttBindingTypeFromName(const QString& name) {
    if (name == "keyboard") {
        return PttBindingType::Keyboard;
    }
    if (name == "mouse") {
        return PttBindingType::Mouse;
    }
    return std::nullopt;
}

Result<QStringList> normalizeStunServers(const QStringList& servers) {
    if (servers.isEmpty()) {
        return Result<QStringList>::failure("At least one STUN URI is required");
    }

    QStringList normalized;
    normalized.reserve(servers.size());
    for (const auto& server : servers) {
        const auto uri = server.trimmed();
        if (!uri.startsWith("stun:") || uri.size() <= QString("stun:").size() ||
            uri.size() > MaxStunUriLength ||
            std::any_of(uri.cbegin(), uri.cend(), [](QChar character) {
                return character.isSpace();
            })) {
            return Result<QStringList>::failure("Every ICE server must be a valid stun: URI");
        }
        if (normalized.contains(uri)) {
            return Result<QStringList>::failure("STUN URIs must not contain duplicates");
        }
        normalized.append(uri);
    }
    return Result<QStringList>::success(std::move(normalized));
}

Result<AudioPreferences> parseAudioPreferences(const QJsonObject& root) {
    AudioPreferences preferences;
    if (!root.contains("audio")) {
        return Result<AudioPreferences>::success(std::move(preferences));
    }
    if (!root.value("audio").isObject()) {
        return Result<AudioPreferences>::failure("audio must be an object");
    }

    const auto audio = root.value("audio").toObject();
    constexpr std::array fields{
        std::pair{"capture_device", QJsonValue::String},
        std::pair{"playback_device", QJsonValue::String},
        std::pair{"echo_cancellation", QJsonValue::Bool},
        std::pair{"high_pass_filter", QJsonValue::Bool},
        std::pair{"noise_suppression", QJsonValue::Bool},
        std::pair{"noise_suppression_level", QJsonValue::String},
        std::pair{"automatic_gain_control", QJsonValue::Bool},
        std::pair{"agc_target_level_dbfs", QJsonValue::Double},
        std::pair{"agc_compression_gain_db", QJsonValue::Double},
        std::pair{"agc_limiter", QJsonValue::Bool},
        std::pair{"output_volume", QJsonValue::Double},
        std::pair{"quality_kbps", QJsonValue::Double},
        std::pair{"input_mode", QJsonValue::String},
        std::pair{"vad_threshold", QJsonValue::Double},
        std::pair{"vad_hangover_ms", QJsonValue::Double},
        std::pair{"ptt_binding", QJsonValue::Object},
    };
    for (const auto& [name, type] : fields) {
        const auto key = QString::fromLatin1(name);
        if (audio.contains(key) && audio.value(key).type() != type) {
            return Result<AudioPreferences>::failure("audio." + key + " has an invalid type");
        }
    }

    preferences.captureDevice = audio.value("capture_device").toString();
    preferences.playbackDevice = audio.value("playback_device").toString();
    preferences.echoCancellation = audio.value("echo_cancellation").toBool(false);
    preferences.highPassFilter = audio.value("high_pass_filter").toBool(true);
    preferences.noiseSuppression = audio.value("noise_suppression").toBool(true);
    preferences.automaticGainControl = audio.value("automatic_gain_control").toBool(true);
    preferences.agcLimiter = audio.value("agc_limiter").toBool(true);

    const auto inputMode = inputModeFromName(audio.value("input_mode").toString("voice_activity"));
    const auto noiseSuppressionLevel = noiseSuppressionLevelFromName(
        audio.value("noise_suppression_level").toString("moderate"));
    const auto outputVolume = audio.value("output_volume").toDouble(100);
    const auto qualityKbps = audio.value("quality_kbps").toDouble(48);
    const auto agcTargetLevelDbfs = audio.value("agc_target_level_dbfs").toDouble(3);
    const auto agcCompressionGainDb = audio.value("agc_compression_gain_db").toDouble(9);
    const auto vadThreshold = audio.value("vad_threshold").toDouble(0.15);
    const auto vadHangoverMs = audio.value("vad_hangover_ms").toDouble(200);
    const bool validQuality = qualityKbps == 24 || qualityKbps == 32 || qualityKbps == 48;
    if (!inputMode || !noiseSuppressionLevel ||
        !isIntegerInRange(outputVolume, 0, 200) || !validQuality ||
        !isIntegerInRange(agcTargetLevelDbfs, 0, 31) ||
        !isIntegerInRange(agcCompressionGainDb, 0, 90) || !std::isfinite(vadThreshold) ||
        vadThreshold < 0.0 || vadThreshold > 1.0 ||
        !isIntegerInRange(vadHangoverMs, 0, 2000)) {
        return Result<AudioPreferences>::failure("Invalid audio settings");
    }
    preferences.outputVolume = static_cast<int>(outputVolume);
    preferences.qualityKbps = static_cast<int>(qualityKbps);
    preferences.agcTargetLevelDbfs = static_cast<int>(agcTargetLevelDbfs);
    preferences.agcCompressionGainDb = static_cast<int>(agcCompressionGainDb);
    preferences.inputMode = *inputMode;
    preferences.noiseSuppressionLevel = *noiseSuppressionLevel;
    preferences.vadThreshold = vadThreshold;
    preferences.vadHangoverMs = static_cast<int>(vadHangoverMs);

    if (audio.contains("ptt_binding")) {
        const auto binding = audio.value("ptt_binding").toObject();
        if (!binding.value("type").isString() || !binding.value("platform").isString() ||
            !binding.value("code").isDouble() || !binding.value("name").isString()) {
            return Result<AudioPreferences>::failure("audio.ptt_binding is invalid");
        }
        const auto bindingType = pttBindingTypeFromName(binding.value("type").toString());
        const auto bindingCode = binding.value("code").toDouble(-1);
        if (!bindingType || !isIntegerInRange(bindingCode, 0, MaxSerializedPttCode)) {
            return Result<AudioPreferences>::failure("audio.ptt_binding is invalid");
        }
        preferences.pttBinding = {*bindingType, binding.value("platform").toString(),
                                  static_cast<quint32>(bindingCode),
                                  binding.value("name").toString()};
    }
    if (!preferences.isValid()) {
        return Result<AudioPreferences>::failure("Invalid audio settings");
    }
    return Result<AudioPreferences>::success(std::move(preferences));
}

} // namespace

bool PttBinding::isValid() const {
    const bool validType = type == PttBindingType::Keyboard || type == PttBindingType::Mouse;
    if (!validType || displayName.trimmed().isEmpty() ||
        displayName.size() > MaxPttBindingNameLength) {
        return false;
    }

    if (platform.isEmpty()) {
        return type == PttBindingType::Keyboard && code == 'V';
    }
    if (platform == "windows") {
        if (type == PttBindingType::Keyboard) {
            return code > 0 && code <= MaxWindowsVirtualKey && !isWindowsMouseVirtualKey(code);
        }
        return code == WindowsMiddleMouseButton || code == WindowsBackMouseButton ||
               code == WindowsForwardMouseButton;
    }
    if (platform == "macos") {
        if (type == PttBindingType::Keyboard) {
            return code <= MaxMacVirtualKey;
        }
        return code == MacMiddleMouseButton || code == MacBackMouseButton ||
               code == MacForwardMouseButton;
    }
    return false;
}

bool AudioPreferences::isValid() const {
    const bool validInputMode = inputMode == AudioInputMode::VoiceActivity ||
                                inputMode == AudioInputMode::PushToTalk;
    const bool validNoiseSuppressionLevel =
        noiseSuppressionLevel == NoiseSuppressionLevel::Low ||
        noiseSuppressionLevel == NoiseSuppressionLevel::Moderate ||
        noiseSuppressionLevel == NoiseSuppressionLevel::High ||
        noiseSuppressionLevel == NoiseSuppressionLevel::VeryHigh;
    if (!validInputMode || !validNoiseSuppressionLevel) {
        return false;
    }
    if (outputVolume < 0 || outputVolume > 200 ||
        (qualityKbps != 24 && qualityKbps != 32 && qualityKbps != 48)) {
        return false;
    }
    if (agcTargetLevelDbfs < 0 || agcTargetLevelDbfs > 31 || agcCompressionGainDb < 0 ||
        agcCompressionGainDb > 90) {
        return false;
    }
    if (!std::isfinite(vadThreshold) || vadThreshold < 0.0 || vadThreshold > 1.0 ||
        vadHangoverMs < 0 || vadHangoverMs > 2000) {
        return false;
    }
    return pttBinding.isValid();
}

Result<AppConfig> AppConfig::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return Result<AppConfig>::failure("Cannot open configuration: " + f.errorString());
    }
    QJsonParseError e;
    const auto d = QJsonDocument::fromJson(f.readAll(), &e);
    if (e.error != QJsonParseError::NoError) {
        return Result<AppConfig>::failure("Invalid configuration JSON: " + e.errorString());
    }
    if (!d.isObject()) {
        return Result<AppConfig>::failure("Configuration root must be an object");
    }

    AppConfig c;
    const auto root = d.object();
    const auto serverValue = root.value("stun_servers");
    if (!serverValue.isArray()) {
        return Result<AppConfig>::failure("stun_servers must be an array");
    }

    QStringList servers;
    for (const auto& value : serverValue.toArray()) {
        if (!value.isString()) {
            return Result<AppConfig>::failure("Every stun_servers item must be a string");
        }
        servers.append(value.toString());
    }
    auto normalizedServers = normalizeStunServers(servers);
    if (!normalizedServers) {
        return Result<AppConfig>::failure(normalizedServers.error());
    }
    c.stunServers = std::move(normalizedServers.value());

    auto audio = parseAudioPreferences(root);
    if (!audio) {
        return Result<AppConfig>::failure(audio.error());
    }
    c.audio = std::move(audio.value());
    return Result<AppConfig>::success(std::move(c));
}

Result<void> AppConfig::save(const QString& path) const {
    auto normalizedServers = normalizeStunServers(stunServers);
    if (!normalizedServers) {
        return Result<void>::failure(normalizedServers.error());
    }
    if (!audio.isValid()) {
        return Result<void>::failure("Audio settings are invalid");
    }

    QJsonArray servers;
    for (const auto& server : normalizedServers.value()) {
        servers.append(server);
    }
    const QJsonObject pttBinding{{"type", pttBindingTypeName(audio.pttBinding.type)},
                                 {"platform", audio.pttBinding.platform},
                                 {"code", static_cast<qint64>(audio.pttBinding.code)},
                                 {"name", audio.pttBinding.displayName}};
    const QJsonObject audioObject{
        {"capture_device", audio.captureDevice},
        {"playback_device", audio.playbackDevice},
        {"echo_cancellation", audio.echoCancellation},
        {"high_pass_filter", audio.highPassFilter},
        {"noise_suppression", audio.noiseSuppression},
        {"noise_suppression_level", noiseSuppressionLevelName(audio.noiseSuppressionLevel)},
        {"automatic_gain_control", audio.automaticGainControl},
        {"agc_target_level_dbfs", audio.agcTargetLevelDbfs},
        {"agc_compression_gain_db", audio.agcCompressionGainDb},
        {"agc_limiter", audio.agcLimiter},
        {"output_volume", audio.outputVolume},
        {"quality_kbps", audio.qualityKbps},
        {"input_mode", inputModeName(audio.inputMode)},
        {"vad_threshold", audio.vadThreshold},
        {"vad_hangover_ms", audio.vadHangoverMs},
        {"ptt_binding", pttBinding}};
    const QJsonObject root{{"stun_servers", servers}, {"audio", audioObject}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return Result<void>::failure("Cannot write configuration: " + file.errorString());
    }
    const auto contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        return Result<void>::failure("Cannot save configuration: " + file.errorString());
    }
    return Result<void>::success();
}

} // namespace tmc
