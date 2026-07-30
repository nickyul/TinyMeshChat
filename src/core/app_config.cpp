#include "tmc/core/app_config.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>

namespace tmc {

namespace {

constexpr qsizetype MaxStunUriLength = 512;

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
        std::pair{"noise_suppression", QJsonValue::Bool},
        std::pair{"automatic_gain_control", QJsonValue::Bool},
        std::pair{"output_volume", QJsonValue::Double},
        std::pair{"quality_kbps", QJsonValue::Double},
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
    preferences.noiseSuppression = audio.value("noise_suppression").toBool(true);
    preferences.automaticGainControl = audio.value("automatic_gain_control").toBool(true);
    const auto outputVolume = audio.value("output_volume").toDouble(100);
    const auto qualityKbps = audio.value("quality_kbps").toDouble(48);
    if (outputVolume != std::floor(outputVolume) || outputVolume < 0 || outputVolume > 200 ||
        (qualityKbps != 24 && qualityKbps != 32 && qualityKbps != 48)) {
        return Result<AudioPreferences>::failure("Invalid audio settings");
    }
    preferences.outputVolume = static_cast<int>(outputVolume);
    preferences.qualityKbps = static_cast<int>(qualityKbps);
    if (!preferences.isValid()) {
        return Result<AudioPreferences>::failure("Invalid audio settings");
    }
    return Result<AudioPreferences>::success(std::move(preferences));
}

} // namespace

bool AudioPreferences::isValid() const {
    return outputVolume >= 0 && outputVolume <= 200 &&
           (qualityKbps == 24 || qualityKbps == 32 || qualityKbps == 48);
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
    const QJsonObject audioObject{{"capture_device", audio.captureDevice},
                                  {"playback_device", audio.playbackDevice},
                                  {"echo_cancellation", audio.echoCancellation},
                                  {"noise_suppression", audio.noiseSuppression},
                                  {"automatic_gain_control", audio.automaticGainControl},
                                  {"output_volume", audio.outputVolume},
                                  {"quality_kbps", audio.qualityKbps}};
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
