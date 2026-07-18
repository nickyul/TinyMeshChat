#include "tmc/core/app_config.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace tmc {

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
    if (e.error != QJsonParseError::NoError || !d.isObject()) {
        return Result<AppConfig>::failure("Invalid configuration JSON: " + e.errorString());
    }
    AppConfig c;
    const auto root = d.object();
    auto serverValue = root.value("stun_servers");
    if (!serverValue.isArray()) {
        serverValue = root.value("ice").toObject().value("stun_servers");
    }
    if (!serverValue.isArray()) {
        return Result<AppConfig>::failure("stun_servers must be an array");
    }
    c.stunServers.clear();
    for (const auto& value : serverValue.toArray()) {
        if (!value.isString()) {
            continue;
        }
        const auto server = value.toString().trimmed();
        if (server.startsWith("stun:") && server.size() <= 512 && !c.stunServers.contains(server)) {
            c.stunServers.append(server);
        }
    }
    if (c.stunServers.isEmpty()) {
        return Result<AppConfig>::failure("At least one STUN URI is required");
    }
    const auto audio = root.value("audio").toObject();
    if (!audio.isEmpty()) {
        c.audio.captureDevice = audio.value("capture_device").toString();
        c.audio.playbackDevice = audio.value("playback_device").toString();
        c.audio.echoCancellation = audio.value("echo_cancellation").toBool(true);
        c.audio.noiseSuppression = audio.value("noise_suppression").toBool(true);
        c.audio.automaticGainControl = audio.value("automatic_gain_control").toBool(true);
        c.audio.outputVolume = audio.value("output_volume").toInt(100);
        c.audio.qualityKbps = audio.value("quality_kbps").toInt(32);
        if (!c.audio.isValid()) {
            return Result<AppConfig>::failure("Invalid audio settings");
        }
    }
    return Result<AppConfig>::success(c);
}

Result<void> AppConfig::save(const QString& path) const {
    if (stunServers.isEmpty()) {
        return Result<void>::failure("At least one STUN URI is required");
    }
    if (!audio.isValid()) {
        return Result<void>::failure("Audio settings are invalid");
    }
    QJsonArray servers;
    for (const auto& server : stunServers) {
        const auto normalized = server.trimmed();
        if (!normalized.startsWith("stun:") || normalized.size() > 512) {
            return Result<void>::failure("Every ICE server must be a valid stun: URI");
        }
        servers.append(normalized);
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
